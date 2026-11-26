#include "stdafx.h"
#pragma hdrstop

#include "xr_input.h"
#include "IInputReceiver.h"
//#include "../include/editor/ide.hpp"

#include "xr_input_xinput.h"
#include "xr_input_hid.h"
CInput* pInput = NULL;
IInputReceiver dummyController;

ENGINE_API float psMouseSens = 1.f;
ENGINE_API float psMouseSensScale = 1.f;
ENGINE_API float psMouseSensVerticalK = 1.f;
// mouse_filter 0/1: the camera turns by the average of this and the last frame's mouse movement (Quake's m_filter). For
// mouse updates that don't line up with the frames: under Wine on macOS about one per display refresh reaches the game,
// so a frame got one or two and the camera turned by uneven steps (felt as stutter). Costs half a frame of delay
ENGINE_API int psMouseFilter = 0;
ENGINE_API Flags32 psMouseInvert = {FALSE};
// gp_vibration: the gamepad's motors 0..1 of what the game asks
ENGINE_API float psGamepadVibration = 1.f;
// gp_sony: Sony controllers (DualSense, DualShock 4) read as HID, with the gyro, before XInput
ENGINE_API int psGamepadSony = 1;

// demonized: configurable buffer sizes for mouse and kb
int MOUSEBUFFERSIZE = 1024;
int KEYBOARDBUFFERSIZE = 128;

#define _KEYDOWN(name,key) ( name[key] & 0x80 )

static bool g_exclusive = false;
extern u32 g_screenmode;

static void on_error_dialog(bool before)
{
#ifdef INGAME_EDITOR
    if (Device.editor())
        return;
#endif // #ifdef INGAME_EDITOR
	if (!pInput || !g_exclusive)
		return;

	if (before)
	{
		pInput->unacquire();
		return;
	}

	pInput->acquire(true);
}

CInput::CInput(BOOL bExclusive, int deviceForInit)
{
	g_exclusive = !!bExclusive;

	Log("Starting INPUT device...");

	pDI = NULL;
	pMouse = NULL;
	pKeyboard = NULL;

	//=====================Mouse
	mouse_property.mouse_dt = 25;

	ZeroMemory(mouseState, sizeof(mouseState));
	ZeroMemory(KBState, sizeof(KBState));
	ZeroMemory(padKeyState, sizeof(padKeyState));
	ZeroMemory(timeStamp, sizeof(timeStamp));
	ZeroMemory(timeSave, sizeof(timeStamp));
	ZeroMemory(offs, sizeof(offs));
	ZeroMemory(filter_prev, sizeof(filter_prev));
	ZeroMemory(filter_rest, sizeof(filter_rest));

	//===================== Dummy pack
	iCapture(&dummyController);

	if (!pDI)
		CHK_DX(DirectInput8Create(GetModuleHandle(NULL), DIRECTINPUT_VERSION, IID_IDirectInput8, (void**)&pDI, NULL));

	//. u32 kb_input_flags = ((bExclusive)?DISCL_EXCLUSIVE:DISCL_NONEXCLUSIVE) | DISCL_FOREGROUND;
	u32 kb_input_flags = ((bExclusive) ? DISCL_EXCLUSIVE : DISCL_NONEXCLUSIVE) | DISCL_FOREGROUND;

	//. u32 mouse_input_flags = ((bExclusive)?DISCL_EXCLUSIVE:DISCL_NONEXCLUSIVE) | DISCL_FOREGROUND | DISCL_NOWINKEY,
	u32 mouse_input_flags = ((bExclusive) ? DISCL_EXCLUSIVE : DISCL_NONEXCLUSIVE) | DISCL_FOREGROUND | DISCL_NOWINKEY;

	// KEYBOARD
	if (deviceForInit & keyboard_device_key)
		CHK_DX(CreateInputDevice(
		&pKeyboard, GUID_SysKeyboard, &c_dfDIKeyboard,
		kb_input_flags,
		KEYBOARDBUFFERSIZE));

	// MOUSE
	if (deviceForInit & mouse_device_key)
		CHK_DX(CreateInputDevice(
		&pMouse, GUID_SysMouse, &c_dfDIMouse2,
		mouse_input_flags,
		MOUSEBUFFERSIZE));

	Debug.set_on_dialog(&on_error_dialog);

#ifdef ENGINE_BUILD
	Device.seqAppActivate.Add(this);
	Device.seqAppDeactivate.Add(this, REG_PRIORITY_HIGH);
	Device.seqFrame.Add(this, REG_PRIORITY_HIGH);
#endif
}

CInput::~CInput(void)
{
	sonypad::shutdown();
#ifdef ENGINE_BUILD
	Device.seqFrame.Remove(this);
	Device.seqAppDeactivate.Remove(this);
	Device.seqAppActivate.Remove(this);
#endif
	//_______________________

	// Unacquire and release the device's interfaces
	if (pMouse)
	{
		pMouse->Unacquire();
		_RELEASE(pMouse);
	}

	if (pKeyboard)
	{
		pKeyboard->Unacquire();
		_RELEASE(pKeyboard);
	}

	_SHOW_REF("Input: ", pDI);
	_RELEASE(pDI);
}

//-----------------------------------------------------------------------------
// Name: CreateInputDevice()
// Desc: Create a DirectInput device.
//-----------------------------------------------------------------------------
HRESULT CInput::CreateInputDevice(LPDIRECTINPUTDEVICE8* device, GUID guidDevice, const DIDATAFORMAT* pdidDataFormat,
                                  u32 dwFlags, u32 buf_size)
{
	// Obtain an interface to the input device
	//. CHK_DX( pDI->CreateDeviceEx( guidDevice, IID_IDirectInputDevice8, (void**)device, NULL ) );
	CHK_DX(pDI->CreateDevice(guidDevice, /*IID_IDirectInputDevice8,*/ device, NULL));

	// Set the device data format. Note: a data format specifies which
	// controls on a device we are interested in, and how they should be
	// reported.
	CHK_DX((*device)->SetDataFormat(pdidDataFormat));

	// Set the cooperativity level to let DirectInput know how this device
	// should interact with the system and with other DirectInput applications.
#ifdef INGAME_EDITOR
    if (!Device.editor())
#endif // #ifdef INGAME_EDITOR
	{
		HRESULT _hr = (*device)->SetCooperativeLevel(RDEVICE.m_hWnd, dwFlags);
		if (FAILED(_hr) && (_hr == E_NOTIMPL)) Msg("! INPUT: Can't set coop level. Emulation???");
		else
			R_CHK(_hr);
	}

	// setup the buffer size for the keyboard data
	DIPROPDWORD dipdw;
	dipdw.diph.dwSize = sizeof(DIPROPDWORD);
	dipdw.diph.dwHeaderSize = sizeof(DIPROPHEADER);
	dipdw.diph.dwObj = 0;
	dipdw.diph.dwHow = DIPH_DEVICE;
	dipdw.dwData = buf_size;

	CHK_DX((*device)->SetProperty(DIPROP_BUFFERSIZE, &dipdw.diph));

	return S_OK;
}

//-----------------------------------------------------------------------

void CInput::SetAllAcquire(BOOL bAcquire)
{
	if (pMouse) bAcquire ? pMouse->Acquire() : pMouse->Unacquire();
	if (pKeyboard) bAcquire ? pKeyboard->Acquire() : pKeyboard->Unacquire();
}

void CInput::SetMouseAcquire(BOOL bAcquire)
{
	if (pMouse) bAcquire ? pMouse->Acquire() : pMouse->Unacquire();
}

void CInput::SetKBDAcquire(BOOL bAcquire)
{
	if (pKeyboard) bAcquire ? pKeyboard->Acquire() : pKeyboard->Unacquire();
}

//-----------------------------------------------------------------------
BOOL b_altF4 = FALSE;

void CInput::KeyUpdate()
{
	if (b_altF4) return;

	HRESULT hr;
	DWORD dwElements = KEYBOARDBUFFERSIZE;
	auto od = std::make_unique<DIDEVICEOBJECTDATA[]>(KEYBOARDBUFFERSIZE);
	DWORD key = 0;

	VERIFY(pKeyboard);

	hr = pKeyboard->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), &od[0], &dwElements, 0);
	if ((hr == DIERR_INPUTLOST) || (hr == DIERR_NOTACQUIRED))
	{
		hr = pKeyboard->Acquire();
		if (hr != S_OK)
			return;

		hr = pKeyboard->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), &od[0], &dwElements, 0);
		if (hr != S_OK)
			return;
	}

	bool b_dik_pause_was_pressed = false;
	if (dwElements)
		m_pad_in_use = false;
	for (u32 idx = 0; idx < dwElements; idx++)
	{
		if (od[idx].dwOfs == DIK_PAUSE)
		{
			if (od[idx].dwData & 0x80)
				b_dik_pause_was_pressed = true;

			if (b_dik_pause_was_pressed && !(od[idx].dwData & 0x80))
			{
				od[idx].uAppData = 666;
				continue; //skip one-frame pause key on-off switch
			}
		}
		KBState[od[idx].dwOfs] = od[idx].dwData & 0x80;
	}

#ifndef _EDITOR
	bool b_alt_tab = false;

	if (!b_altF4 && KBState[DIK_F4] && (KBState[DIK_RMENU] || KBState[DIK_LMENU]))
	{
		b_altF4 = TRUE;
		Engine.Event.Defer("KERNEL:disconnect");
		Engine.Event.Defer("KERNEL:quit");
	}


#endif
	if (b_altF4) return;

#ifndef _EDITOR
	if (Device.dwPrecacheFrame == 0)
#endif
	{
		for (u32 i = 0; i < dwElements; i++)
		{
			if (od[i].uAppData == 666) //ignored action
				continue;

			key = od[i].dwOfs;
			if (od[i].dwData & 0x80)
				cbStack.back()->IR_OnKeyboardPress(key);
			else
			{
				cbStack.back()->IR_OnKeyboardRelease(key);
#ifndef _EDITOR
				if (key == DIK_TAB && (iGetAsyncKeyState(DIK_RMENU) || iGetAsyncKeyState(DIK_LMENU)))
					b_alt_tab = true;
#endif
			}
		}

		for (u32 i = 0; i < COUNT_KB_BUTTONS; i++)
			if (KBState[i])
				cbStack.back()->IR_OnKeyboardHold(i);
	}

#ifndef _EDITOR
	if (b_alt_tab) {
		BOOL fullscreen = (g_screenmode == 2);
		if (fullscreen)
			SendMessage(Device.m_hWnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
	}
#endif
}

void CInput::resetMouseState()
{
	for (int i = 0; i < COUNT_MOUSE_BUTTONS; i++) {
		mouseState[i] = 0;
	}
}

bool CInput::get_dik_name(int dik, LPSTR dest_str, int dest_sz)
{
	DIPROPSTRING keyname;
	keyname.diph.dwSize = sizeof(DIPROPSTRING);
	keyname.diph.dwHeaderSize = sizeof(DIPROPHEADER);
	keyname.diph.dwObj = static_cast<DWORD>(dik);
	keyname.diph.dwHow = DIPH_BYOFFSET;
	HRESULT hr = pKeyboard->GetProperty(DIPROP_KEYNAME, &keyname.diph);
	if (FAILED(hr))
		return false;

	const wchar_t* wct = keyname.wsz;
	if (0 == wcslen(wct))
		return false;

	int cnt = WideCharToMultiByte(CP_ACP, 0, keyname.wsz, -1, dest_str, dest_sz, NULL, NULL);
	if (cnt == -1)
	{
		Msg("! cant convert dik_name for dik[%d], prop=[%S]", dik, keyname.wsz);
		return false;
	}
	return (cnt != -1);
}

bool CInput::dik_to_text(int dik, bool shift, bool caps, bool ctrl, bool alt, bool altgr, LPSTR dest, int dest_sz)
{
	if (!dest || dest_sz <= 1)
		return false;

	dest[0] = 0;

	const UINT scan = static_cast<UINT>(dik);
	const HKL hkl = GetKeyboardLayout(0);

	// Convert scan code -> virtual key according to current layout
	const UINT vk = MapVirtualKeyEx(scan, MAPVK_VSC_TO_VK_EX, hkl);
	if (vk == 0)
		return false;

	BYTE ks[256] = {};
	if (shift) ks[VK_SHIFT] = 0x80;
	if (ctrl)  ks[VK_CONTROL] = 0x80;
	if (alt)   ks[VK_MENU] = 0x80;
	if (caps)  ks[VK_CAPITAL] = 0x01;

	// AltGr (usually Right Alt) on Windows behaves like Ctrl+Alt
	if (altgr)
	{
		ks[VK_CONTROL] = 0x80;
		ks[VK_MENU] = 0x80;
	}
    
	wchar_t wbuf[8] = {};
	const int rc = ToUnicodeEx(vk, scan, ks, wbuf, (int)std::size(wbuf), 0, hkl);

	if (rc <= 0)
	{
		// Dead key: clear keyboard state in ToUnicodeEx internal buffer
		if (rc == -1)
		{
			BYTE ks0[256] = {};
			wchar_t dummy[8] = {};
			ToUnicodeEx(vk, scan, ks0, dummy, (int)std::size(dummy), 0, hkl);
		}
		return false;
	}

	// Filter control chars
	if (wbuf[0] < 0x20)
		return false;

	// Convert produced Unicode to CP1251
	BOOL usedDefault = FALSE;
	const int mb = WideCharToMultiByte(1251, WC_NO_BEST_FIT_CHARS, wbuf, rc, dest, dest_sz - 1, "?", &usedDefault);
	if (mb <= 0)
		return false;

	// Null-terminate
	dest[mb] = 0;

	// If UI cannot represent it in CP1251, prevent cyrillic output
	// Return true to insert '?'
	if (usedDefault)
		return false;

	return true;
}

#define MOUSE_1 (0xED + 100)
#define MOUSE_8 (0xED + 107)

BOOL CInput::iGetAsyncKeyState(int dik)
{
	if (dik < COUNT_KB_BUTTONS)
		return !!KBState[dik];
	else if (is_gamepad_key(dik))
		return !!padKeyState[dik - GP_FIRST];
	else if (dik >= MOUSE_1 && dik <= MOUSE_8)
	{
		int mk = dik - MOUSE_1;
		return iGetAsyncBtnState(mk);
	}
	else
		return FALSE; //unknown key ???
}

void CInput::inject_key(int dik, bool press)
{
	// The state first: a release while nothing receives input (a load, a menu switching) must still release it, or the
	// key stays held for iGetAsyncKeyState (a reload's end then fires the weapon: CWeaponMagazined::OnAnimationEnd)
	if (is_gamepad_key(dik))
		padKeyState[dik - GP_FIRST] = press;
	else if (dik >= 0 && dik < COUNT_KB_BUTTONS)
		KBState[dik] = press;
	else
		return;
	if (cbStack.empty())
		return;
	if (press)
		cbStack.back()->IR_OnKeyboardPress(dik);
	else
		cbStack.back()->IR_OnKeyboardRelease(dik);
}

void CInput::inject_mouse_button(int btn, bool press)
{
	if (btn < 0 || btn >= COUNT_MOUSE_BUTTONS)
		return;
	mouseState[btn] = press; // as in inject_key: the state even with no receiver
	if (cbStack.empty())
		return;
	if (press)
		cbStack.back()->IR_OnMousePress(btn);
	else
		cbStack.back()->IR_OnMouseRelease(btn);
}

void CInput::inject_mouse_move(int dx, int dy)
{
	if (!cbStack.empty())
		cbStack.back()->IR_OnMouseMove(dx, dy);
}

void CInput::inject_mouse_wheel(int direction)
{
	if (!cbStack.empty())
		cbStack.back()->IR_OnMouseWheel(direction);
}

void CInput::release_pad_keys()
{
	for (int i = 0; i < GP_KEY_COUNT; ++i)
		if (padKeyState[i])
			inject_key(GP_FIRST + i, false);
}

void CInput::pad_simulate(const gamepad_state* state)
{
	m_pad_sim_on = state != nullptr;
	if (state)
		m_pad_sim = *state;
}

void CInput::PadUpdate()
{
	gamepad_state now;
	if (m_pad_sim_on)
	{
		// The simulated gyro is a turn rate (radians/s)
		now = m_pad_sim;
		now.gyro_yaw *= Device.fTimeDelta;
		now.gyro_pitch *= Device.fTimeDelta;
	}
	else if (psGamepadSony && sonypad::read(now))
	{
		if (!m_pad_sony)
			m_pad_slot = -1;
		m_pad_sony = true;
	}
	else
	{
		m_pad_sony = false;
		// A missing controller is looked for once a second: XInputGetState on an empty user index is slow
		if (m_pad_slot >= 0 && !xinput::read(m_pad_slot, now))
			m_pad_slot = -1;
		if (m_pad_slot < 0 && dwCurTime - m_pad_scan_time > 1000)
		{
			m_pad_scan_time = dwCurTime;
			for (int slot = 0; slot < 4; ++slot)
				if (xinput::read(slot, now))
				{
					m_pad_slot = slot;
					Msg("* Gamepad connected (XInput user %d)", slot);
					break;
				}
		}
	}
	if (now.connected && (now.lt > 0.3f))
		now.buttons |= 1u << (GP_LT - GP_A);
	if (now.connected && (now.rt > 0.3f))
		now.buttons |= 1u << (GP_RT - GP_A);

	if (m_pad.connected && !now.connected)
	{
		Msg("* Gamepad disconnected");
		release_pad_keys();
	}
	// In use from its first button or a stick pushed past a third (a resting stick drifts a little)
	if (now.connected && ((now.buttons & ~m_pad.buttons) || _abs(now.lx) > 0.33f || _abs(now.ly) > 0.33f ||
		_abs(now.rx) > 0.33f || _abs(now.ry) > 0.33f))
		m_pad_in_use = true;
	m_pad = now;

	if (m_pad_vibrating && dwCurTime >= m_pad_vibration_until)
	{
		m_pad_vibrating = false;
		if (m_pad_sony)
			sonypad::vibrate(0.f, 0.f);
		else
			xinput::vibrate(m_pad_slot, 0.f, 0.f);
	}
}

BOOL CInput::iGetAsyncBtnState(int btn)
{
	if ((btn == 0 || btn == 1) && !!GetSystemMetrics(SM_SWAPBUTTON))
		btn = btn == 0 ? 1 : 0;
	return !!mouseState[btn];
}

void CInput::MouseUpdate()
{
	HRESULT hr;
	DWORD dwElements = MOUSEBUFFERSIZE;
	auto od = std::make_unique<DIDEVICEOBJECTDATA[]>(MOUSEBUFFERSIZE);

	VERIFY(pMouse);

	const u64 t_read = CPU::QPC();
	hr = pMouse->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), &od[0], &dwElements, 0);
	if ((hr == DIERR_INPUTLOST) || (hr == DIERR_NOTACQUIRED))
	{
		hr = pMouse->Acquire();
		if (hr != S_OK) return;
		hr = pMouse->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), &od[0], &dwElements, 0);
		if (hr != S_OK) return;
	};
	g_mouse_events = dwElements; // stat_frame_pacing
	g_input_mouse_read_ms = float(double(CPU::QPC() - t_read) * 1000.0 / double(CPU::qpc_freq));

#ifndef _EDITOR
	if (Device.dwPrecacheFrame)
		return;
#endif
	BOOL mouse_prev[COUNT_MOUSE_BUTTONS];

	mouse_prev[0] = mouseState[0];
	mouse_prev[1] = mouseState[1];
	mouse_prev[2] = mouseState[2];
	mouse_prev[3] = mouseState[3];
	mouse_prev[4] = mouseState[4];
	mouse_prev[5] = mouseState[5];
	mouse_prev[6] = mouseState[6];
	mouse_prev[7] = mouseState[7];

	bool bSwitched = !!GetSystemMetrics(SM_SWAPBUTTON);

	offs[0] = offs[1] = offs[2] = 0;
	for (u32 i = 0; i < dwElements; i++)
	{
		if (od[i].dwOfs >= DIMOFS_BUTTON0 && od[i].dwOfs <= DIMOFS_BUTTON7)
			m_pad_in_use = false;
		switch (od[i].dwOfs)
		{
		case DIMOFS_X:
			offs[0] += od[i].dwData;
			timeStamp[0] = od[i].dwTimeStamp;
			break;
		case DIMOFS_Y:
			offs[1] += od[i].dwData;
			timeStamp[1] = od[i].dwTimeStamp;
			break;
		case DIMOFS_Z:
			offs[2] += od[i].dwData;
			timeStamp[2] = od[i].dwTimeStamp;
			break;
		case DIMOFS_BUTTON0:
			if (od[i].dwData & 0x80)
			{
				mouseState[0] = TRUE;
				cbStack.back()->IR_OnMousePress(bSwitched ? 1 : 0);
			}
			if (!(od[i].dwData & 0x80))
			{
				mouseState[0] = FALSE;
				cbStack.back()->IR_OnMouseRelease(bSwitched ? 1 : 0);
			}
			break;
		case DIMOFS_BUTTON1:
			if (od[i].dwData & 0x80)
			{
				mouseState[1] = TRUE;
				cbStack.back()->IR_OnMousePress(bSwitched ? 0 : 1);
			}
			if (!(od[i].dwData & 0x80))
			{
				mouseState[1] = FALSE;
				cbStack.back()->IR_OnMouseRelease(bSwitched ? 0 : 1);
			}
			break;
		case DIMOFS_BUTTON2:
			if (od[i].dwData & 0x80)
			{
				mouseState[2] = TRUE;
				cbStack.back()->IR_OnMousePress(2);
			}
			if (!(od[i].dwData & 0x80))
			{
				mouseState[2] = FALSE;
				cbStack.back()->IR_OnMouseRelease(2);
			}
			break;
		case DIMOFS_BUTTON3:
			if (od[i].dwData & 0x80)
			{
				mouseState[3] = TRUE;
				cbStack.back()->IR_OnMousePress(3);
			}
			if (!(od[i].dwData & 0x80))
			{
				mouseState[3] = FALSE;
				cbStack.back()->IR_OnMouseRelease(3);
			}
			break;
		case DIMOFS_BUTTON4:
			if (od[i].dwData & 0x80)
			{
				mouseState[4] = TRUE;
				cbStack.back()->IR_OnMousePress(4);
			}
			if (!(od[i].dwData & 0x80))
			{
				mouseState[4] = FALSE;
				cbStack.back()->IR_OnMouseRelease(4);
			}
			break;
		case DIMOFS_BUTTON5:
			if (od[i].dwData & 0x80)
			{
				mouseState[5] = TRUE;
				cbStack.back()->IR_OnMousePress(5);
			}
			if (!(od[i].dwData & 0x80))
			{
				mouseState[5] = FALSE;
				cbStack.back()->IR_OnMouseRelease(5);
			}
			break;
		case DIMOFS_BUTTON6:
			if (od[i].dwData & 0x80)
			{
				mouseState[6] = TRUE;
				cbStack.back()->IR_OnMousePress(6);
			}
			if (!(od[i].dwData & 0x80))
			{
				mouseState[6] = FALSE;
				cbStack.back()->IR_OnMouseRelease(6);
			}
			break;
		case DIMOFS_BUTTON7:
			if (od[i].dwData & 0x80)
			{
				mouseState[7] = TRUE;
				cbStack.back()->IR_OnMousePress(7);
			}
			if (!(od[i].dwData & 0x80))
			{
				mouseState[7] = FALSE;
				cbStack.back()->IR_OnMouseRelease(7);
			}
			break;
		}
	}

	if (mouseState[0] && mouse_prev[0])
	{
		cbStack.back()->IR_OnMouseHold(bSwitched ? 1 : 0);
	}

	if (mouseState[1] && mouse_prev[1])
	{
		cbStack.back()->IR_OnMouseHold(bSwitched ? 0 : 1);
	}

	if (mouseState[2] && mouse_prev[2])
	{
		cbStack.back()->IR_OnMouseHold(2);
	}

	if (mouseState[3] && mouse_prev[3])
	{
		cbStack.back()->IR_OnMouseHold(3);
	}

	if (mouseState[4] && mouse_prev[4])
	{
		cbStack.back()->IR_OnMouseHold(4);
	}

	if (mouseState[5] && mouse_prev[5])
	{
		cbStack.back()->IR_OnMouseHold(5);
	}

	if (mouseState[6] && mouse_prev[6])
	{
		cbStack.back()->IR_OnMouseHold(6);
	}

	if (mouseState[7] && mouse_prev[7])
	{
		cbStack.back()->IR_OnMouseHold(7);
	}

	// mouse_filter: half of this frame's movement and half of the last frame's, also in a frame without new movement;
	// the fractions carry over, so the camera ends up where it would have
	int move[2] = { offs[0], offs[1] };
	if (psMouseFilter)
	{
		for (int axis = 0; axis < 2; ++axis)
		{
			const float want = 0.5f * float(offs[axis] + filter_prev[axis]) + filter_rest[axis];
			move[axis] = int(want); // toward zero, the rest next frame
			filter_rest[axis] = want - float(move[axis]);
			filter_prev[axis] = offs[axis];
		}
	}
	else
	{
		filter_prev[0] = filter_prev[1] = 0;
		filter_rest[0] = filter_rest[1] = 0.f;
	}
	if (move[0] || move[1]) cbStack.back()->IR_OnMouseMove(move[0], move[1]);
	// A real mouse: buttons or more than a nudge (a few counts also come from the cursor the gamepad moves in menus)
	if ((_abs(offs[0]) + _abs(offs[1]) > 6 && dwCurTime - m_pad_cursor_time > 200) || offs[2])
		m_pad_in_use = false;

	if (dwElements)
	{
		if (offs[2]) cbStack.back()->IR_OnMouseWheel(offs[2]);
	}
	else
	{
		if (timeStamp[1] && ((dwCurTime - timeStamp[1]) >= mouse_property.mouse_dt)) cbStack
		                                                                             .back()->IR_OnMouseStop(
			                                                                             DIMOFS_Y, timeStamp[1] = 0);
		if (timeStamp[0] && ((dwCurTime - timeStamp[0]) >= mouse_property.mouse_dt)) cbStack
		                                                                             .back()->IR_OnMouseStop(
			                                                                             DIMOFS_X, timeStamp[0] = 0);
	}
}

//-------------------------------------------------------
void CInput::iCapture(IInputReceiver* p)
{
	VERIFY(p);
	if (pMouse) MouseUpdate();
	if (pKeyboard) KeyUpdate();

	// change focus
	if (!cbStack.empty())
		cbStack.back()->IR_OnDeactivate();
	cbStack.push_back(p);
	cbStack.back()->IR_OnActivate();

	// prepare for _new_ controller
	ZeroMemory(timeStamp, sizeof(timeStamp));
	ZeroMemory(timeSave, sizeof(timeStamp));
	ZeroMemory(offs, sizeof(offs));
}

void CInput::iRelease(IInputReceiver* p)
{
	if (p == cbStack.back())
	{
		cbStack.back()->IR_OnDeactivate();
		cbStack.pop_back();
		IInputReceiver* ir = cbStack.back();
		ir->IR_OnActivate();
	}
	else
	{
		// we are not topmost receiver, so remove the nearest one
		u32 cnt = cbStack.size();
		for (; cnt > 0; --cnt)
			if (cbStack[cnt - 1] == p)
			{
				xr_vector<IInputReceiver*>::iterator it = cbStack.begin();
				std::advance(it, cnt - 1);
				cbStack.erase(it);
				break;
			}
	}
}

void CInput::OnAppActivate(void)
{
	if (CurrentIR())
		CurrentIR()->IR_OnActivate();

	SetAllAcquire(true);
	ZeroMemory(mouseState, sizeof(mouseState));
	ZeroMemory(KBState, sizeof(KBState));
	ZeroMemory(timeStamp, sizeof(timeStamp));
	ZeroMemory(timeSave, sizeof(timeStamp));
	ZeroMemory(offs, sizeof(offs));
}

void CInput::OnAppDeactivate(void)
{
	release_pad_keys();
	if (CurrentIR())
		CurrentIR()->IR_OnDeactivate();

	SetAllAcquire(false);
	ZeroMemory(mouseState, sizeof(mouseState));
	ZeroMemory(KBState, sizeof(KBState));
	ZeroMemory(timeStamp, sizeof(timeStamp));
	ZeroMemory(timeSave, sizeof(timeStamp));
	ZeroMemory(offs, sizeof(offs));
}

void CInput::DeactivateSoft()
{
	if (CurrentIR())
		CurrentIR()->IR_OnDeactivate();

	ZeroMemory(mouseState, sizeof(mouseState));
	ZeroMemory(KBState, sizeof(KBState));
	ZeroMemory(timeStamp, sizeof(timeStamp));
	ZeroMemory(timeSave, sizeof(timeStamp));
	ZeroMemory(offs, sizeof(offs));
}

void CInput::OnFrame(void)
{
	PROF_EVENT("CInput::OnFrame");
	RDEVICE.Statistic->Input.Begin();
	dwCurTime = RDEVICE.TimerAsync_MMT();
	const u64 t_start = CPU::QPC();
	if (pKeyboard) KeyUpdate();
	const u64 t_kb = CPU::QPC();
	g_input_mouse_read_ms = 0.f;
	if (pMouse) MouseUpdate();
	{
		PROF_EVENT("gamepad");
		PadUpdate();
		// Real time: the game's is 0 while paused (the main menu over a level, where the stick moved the cursor by 0)
		static u64 last_pad_frame = 0;
		const u64 now = CPU::QPC();
		const float dt = last_pad_frame ? _min(float(double(now - last_pad_frame) / double(CPU::qpc_freq)), 0.1f) : 0.f;
		last_pad_frame = now;
#ifndef _EDITOR
		if (Device.dwPrecacheFrame == 0)
#endif
		{
			if (pad_handler)
				pad_handler(dt);
			for (int i = 0; i < GP_KEY_COUNT; ++i)
				if (padKeyState[i] && !cbStack.empty())
					cbStack.back()->IR_OnKeyboardHold(GP_FIRST + i);
		}
	}
	const double to_ms = 1000.0 / double(CPU::qpc_freq);
	g_input_kb_ms = float(double(t_kb - t_start) * to_ms);
	g_input_mouse_ms = float(double(CPU::QPC() - t_kb) * to_ms); // stat_frame_pacing
	RDEVICE.Statistic->Input.End();
}

IInputReceiver* CInput::CurrentIR()
{
	if (cbStack.size())
		return cbStack.back();
	else
		return NULL;
}

void CInput::unacquire()
{
	pKeyboard->Unacquire();
	pMouse->Unacquire();
}

void CInput::acquire(const bool& exclusive)
{
	pKeyboard->SetCooperativeLevel(
#ifdef INGAME_EDITOR
        Device.editor() ? Device.editor()->main_handle() :
#endif // #ifdef INGAME_EDITOR
		RDEVICE.m_hWnd,
		(exclusive ? DISCL_EXCLUSIVE : DISCL_NONEXCLUSIVE) | DISCL_FOREGROUND
	);
	pKeyboard->Acquire();

	pMouse->SetCooperativeLevel(
#ifdef INGAME_EDITOR
        Device.editor() ? Device.editor()->main_handle() :
#endif // #ifdef INGAME_EDITOR
		RDEVICE.m_hWnd,
		(exclusive ? DISCL_EXCLUSIVE : DISCL_NONEXCLUSIVE) | DISCL_FOREGROUND | DISCL_NOWINKEY
	);
	pMouse->Acquire();
}

void CInput::exclusive_mode(const bool& exclusive)
{
	g_exclusive = exclusive;
	unacquire();
	acquire(exclusive);
}

bool CInput::get_exclusive_mode()
{
	return g_exclusive;
}

void CInput::feedback(u16 s1, u16 s2, float time)
{
	if (!m_pad.connected || m_pad_sim_on || psGamepadVibration <= 0.f)
		return;
	const float low = psGamepadVibration * float(s1) / 65535.f, high = psGamepadVibration * float(s2) / 65535.f;
	if (m_pad_sony)
		sonypad::vibrate(low, high);
	else
		xinput::vibrate(m_pad_slot, low, high);
	m_pad_vibrating = true;
	m_pad_vibration_until = dwCurTime + u32(iFloor(time * 1000.f));
}

void CInput::pad_trigger_effect(int effect)
{
	if (m_pad_sony)
		sonypad::trigger_effect(effect);
}
