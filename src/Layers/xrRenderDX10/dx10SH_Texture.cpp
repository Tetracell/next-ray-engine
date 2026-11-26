#include "stdafx.h"
#pragma hdrstop

#include "../xrRender/ResourceManager.h"

void fix_texture_name(LPSTR fn);

#ifndef _EDITOR
#include "../../xrEngine/render.h"
#endif

#include "../../xrEngine/tntQAVI.h"
#include "../../xrEngine/xrTheora_Surface.h"
#include "../xrRender/gifPlayer.h"

#include "../xrRender/dxRenderDeviceRender.h"

#include "StateManager/dx10ShaderResourceStateCache.h"
#ifdef USE_DX11
#include "../xrRenderPC_R4/r4_gpu_memory.h"
#endif

#define		PRIORITY_HIGH	12
#define		PRIORITY_NORMAL	8
#define		PRIORITY_LOW	4

#ifdef USE_DX11
// Texture streaming (r4_tex_stream): world textures load small with the level (mips skipped until the largest is at
// most 128 px) and whole on their first bind, in the background; until the whole one is in (swapped on the main
// thread between frames) the small one is drawn, so nothing waits. One not bound for r4_tex_stream_keep seconds goes
// back to its small version. Anything that asks for a streamed texture's surface or size gets it whole for good.
// Measured on the Steam Deck: ~0.5 GB of GAMMA's ~2.4 GB of textures were bound in a minute of play
int ps_r4_tex_stream = 1; // read when textures load
int ps_r4_tex_stream_keep = 60;
extern xr_task_group textures_load_tasks;
extern thread_local int g_texture_load_skip;
extern thread_local int g_texture_load_max_dim;
extern thread_local int g_texture_load_lod_normal;

namespace tex_streaming
{
	static xr_vector<ref_texture> s_loading; // whole versions loading
	static xr_vector<ref_texture> s_whole; // whole versions in use
	static u32 s_next_sweep = 0;
	const int kSmallSize = 128;

	// World textures: not UI, fonts, engine textures ($...), shader helper textures (noise, LUTs, sky), lights, or
	// scope reticles (those always load whole, see get_texture_load_lod)
	bool eligible(LPCSTR name)
	{
		string_path n;
		xr_strcpy(n, name);
		strlwr(n);
		if (strchr(n, '$'))
			return false;
		static const char* prefixes[] = { "ui\\", "ui_", "fonts\\", "intro\\", "shaders\\", "fx\\", "pfx\\", "sky\\", "ed\\",
			"lights\\", "lights_", "internal\\" };
		for (const char* p : prefixes)
			if (!strncmp(n, p, xr_strlen(p)))
				return false;
		static const char* parts[] = { "reticle", "crosshair", "bonus_sights", "\\sights\\", "/sights/", "lut", "noise" };
		for (const char* p : parts)
			if (strstr(n, p))
				return false;
		return true;
	}

	void queue(CTexture* T) { s_loading.push_back(ref_texture(T)); }

	// Main thread, every frame: finished whole versions in, unused ones out
	void update()
	{
		for (size_t i = 0; i < s_loading.size();)
		{
			CTexture* T = &*s_loading[i];
			if (T->m_stream == 2 && !T->m_stream_done.load(std::memory_order_acquire))
			{
				i++;
				continue;
			}
			if (T->m_stream == 2)
			{
				T->stream_swap_in();
				if (T->m_stream == 3)
					s_whole.push_back(s_loading[i]);
			}
			s_loading[i] = s_loading.back();
			s_loading.pop_back();
		}

		const u32 now = Device.dwTimeContinual;
		if (now < s_next_sweep)
			return;
		s_next_sweep = now + 1000;
		const u32 keep = u32(_max(ps_r4_tex_stream_keep, 1)) * 1000;
		for (size_t i = 0; i < s_whole.size();)
		{
			CTexture* T = &*s_whole[i];
			if (T->m_stream == 3 && now - T->m_last_used <= keep)
			{
				i++;
				continue;
			}
			if (T->m_stream == 3)
				T->stream_evict();
			s_whole[i] = s_whole.back();
			s_whole.pop_back();
		}
	}

	// Level unload, device destroy: loads finished, every streamed texture back to small
	void clear()
	{
		textures_load_tasks.wait();
		for (ref_texture& T : s_loading)
		{
			if (T->m_stream == 2)
			{
				T->stream_swap_in();
				if (T->m_stream == 3)
					T->stream_evict();
			}
		}
		for (ref_texture& T : s_whole)
		{
			if (T->m_stream == 3)
				T->stream_evict();
		}
		s_loading.clear();
		s_whole.clear();
	}
}

// Background load of the whole version (main thread, from Apply)
void CTexture::stream_request()
{
	m_stream = 2;
	m_stream_done.store(false, std::memory_order_relaxed);
	tex_streaming::queue(this); // keeps it alive until the load is in
	CTexture* T = this;
	textures_load_tasks.run([T]()
	{
		u32 mem = 0;
		g_texture_load_max_dim = 0;
		ID3DBaseTexture* whole = ::RImplementation.texture_load(*T->cName, mem, false);
		T->m_stream_pending_mem = mem;
		T->m_stream_pending_skip = u8(g_texture_load_skip);
		T->m_stream_pending = whole;
		T->m_stream_done.store(true, std::memory_order_release);
	});
}

// Main thread: the loaded whole version in, the small one kept
void CTexture::stream_swap_in()
{
	ID3DBaseTexture* whole = m_stream_pending;
	m_stream_pending = nullptr;
	m_stream_done.store(false, std::memory_order_relaxed);
	if (!whole)
	{
		m_stream = 0; // couldn't load it: stays small
		return;
	}
	if (flags.bLoadedAsStaging)
		ProcessStaging();
	m_stream_small = pSurface;
	if (m_stream_small)
		m_stream_small->AddRef();
	m_stream_small_skip = m_mip_skip;
	m_stream_small_mem = flags.MemoryUsage;
	surface_set(whole);
	whole->Release();
	m_mip_skip = m_stream_pending_skip;
	flags.MemoryUsage = m_stream_pending_mem;
	m_stream = 3;
}

// Main thread: back to the small version
void CTexture::stream_evict()
{
	if (m_stream != 3 || !m_stream_small)
		return;
	surface_set(m_stream_small);
	_RELEASE(m_stream_small);
	m_mip_skip = m_stream_small_skip;
	flags.MemoryUsage = m_stream_small_mem;
	m_stream = 1;
}

// Whole for good (asked for its surface or size)
void CTexture::stream_whole_now()
{
	wait_for_loading();
	if (m_stream == 1)
	{
		if (flags.bLoadedAsStaging)
			ProcessStaging();
		u32 mem = 0;
		g_texture_load_max_dim = 0;
		m_stream_pending = ::RImplementation.texture_load(*cName, mem, false);
		m_stream_pending_mem = mem;
		m_stream_pending_skip = u8(g_texture_load_skip);
		stream_swap_in();
	}
	else if (m_stream == 2)
	{
		while (!m_stream_done.load(std::memory_order_acquire))
			SwitchToThread();
		stream_swap_in();
	}
	_RELEASE(m_stream_small);
	m_stream = 0;
}

// Unload: a background load finished first, both versions released
void CTexture::stream_drop()
{
	if (m_stream == 2)
	{
		while (!m_stream_done.load(std::memory_order_acquire))
			SwitchToThread();
	}
	_RELEASE(m_stream_pending);
	m_stream_done.store(false, std::memory_order_relaxed);
	_RELEASE(m_stream_small);
	m_stream = 0;
}
#endif

// Background PPL task group used to (re)load textures off the calling thread.
// Defined in dx10ResourceManager_Resources.cpp; CResourceManager::_CreateTexture
// dispatches brand-new textures' initial Load() through it whenever the device is
// ready. apply_load() below reuses the same queue for evicted textures instead of
// calling Load() inline.
extern xr_task_group textures_load_tasks;

void resptrcode_texture::create(LPCSTR _name)
{
	PROF_EVENT("resptrcode_texture::create");
	_set(DEV->_CreateTexture(_name));
}


//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////
CTexture::CTexture()
{
	pSurface = NULL;
	m_pSRView = NULL;
	pAVI = NULL;
	pTheora = NULL;
    gifPlayer = nullptr;
	desc_cache = 0;
	ZeroMemory(&desc, sizeof(desc));
	seqMSPF = 0;
	flags.MemoryUsage = 0;
	flags.bLoaded = false;
	flags.bLoading = false;
	flags.bUser = false;
	flags.seqCycles = FALSE;
	flags.bLoadedAsStaging = FALSE;
	m_material = 1.0f;
	bind = xr_make_delegate(this, &CTexture::apply_load);
}

CTexture::~CTexture()
{
	Unload();

	// release external reference
	DEV->_DeleteTexture(this);
}

void CTexture::surface_set(ID3DBaseTexture* surf)
{
	if (flags.bLoading)
	{
		PROF_EVENT("CTexture: wait for its load");
		while (flags.bLoading)
			SwitchToThread();
	}

	if (cName.size() && strstr(cName.c_str(), "$user$"))
		flags.bUser = true;
	if (surf) surf->AddRef();
	_RELEASE(pSurface);
	_RELEASE(m_pSRView);

	pSurface = surf;

	if (pSurface)
	{
#ifdef USE_DX11
		gpu_memory::track(pSurface, *cName);
#endif
		desc_update();

		D3D_RESOURCE_DIMENSION type;
		pSurface->GetType(&type);
		if (D3D_RESOURCE_DIMENSION_TEXTURE2D == type)
		{
			D3D_SHADER_RESOURCE_VIEW_DESC ViewDesc;

			if (desc.MiscFlags & D3D_RESOURCE_MISC_TEXTURECUBE)
			{
				ViewDesc.ViewDimension = D3D_SRV_DIMENSION_TEXTURECUBE;
				ViewDesc.TextureCube.MostDetailedMip = 0;
				ViewDesc.TextureCube.MipLevels = desc.MipLevels;
			}
			else
			{
				if (desc.SampleDesc.Count <= 1)
				{
					ViewDesc.ViewDimension = D3D_SRV_DIMENSION_TEXTURE2D;
					ViewDesc.Texture2D.MostDetailedMip = 0;
					ViewDesc.Texture2D.MipLevels = desc.MipLevels;
				}
				else
				{
					ViewDesc.ViewDimension = D3D_SRV_DIMENSION_TEXTURE2DMS;
					ViewDesc.Texture2D.MostDetailedMip = 0;
					ViewDesc.Texture2D.MipLevels = desc.MipLevels;
				}
			}

			ViewDesc.Format = DXGI_FORMAT_UNKNOWN;

			switch (desc.Format)
			{
			case DXGI_FORMAT_R24G8_TYPELESS:
				ViewDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
				break;
			case DXGI_FORMAT_R32_TYPELESS:
				ViewDesc.Format = DXGI_FORMAT_R32_FLOAT;
				break;
			}

			// this would be supported by DX10.1 but is not needed for stalker
			// if( ViewDesc.Format != DXGI_FORMAT_R24_UNORM_X8_TYPELESS )
			if ((desc.SampleDesc.Count <= 1) || (ViewDesc.Format != DXGI_FORMAT_R24_UNORM_X8_TYPELESS))
				CHK_DX(HW.pDevice->CreateShaderResourceView(pSurface, &ViewDesc, &m_pSRView));
			else
				m_pSRView = 0;
		}
		else
			CHK_DX(HW.pDevice->CreateShaderResourceView(pSurface, NULL, &m_pSRView));
	}
}

ID3DBaseTexture* CTexture::surface_get()
{
#ifdef USE_DX11
	if (m_stream)
		stream_whole_now();
#endif
	if (flags.bLoading)
	{
		PROF_EVENT("CTexture: wait for its load");
		while (flags.bLoading)
			SwitchToThread();
	}
	if (flags.bLoadedAsStaging)
		ProcessStaging();

	if (pSurface) pSurface->AddRef();
	return pSurface;
}

void CTexture::PostLoad()
{
	if (pTheora) bind = xr_make_delegate(this, &CTexture::apply_theora);
	else if (pAVI) bind = xr_make_delegate(this, &CTexture::apply_avi);
	else if (!seqDATA.empty()) bind = xr_make_delegate(this, &CTexture::apply_seq);
	else if (gifPlayer) bind = xr_make_delegate(this, &CTexture::apply_gif);
	else bind = xr_make_delegate(this, &CTexture::apply_normal);
}

void CTexture::apply_load(u32 dwStage)
{
    if (!flags.bLoaded)
    {
        // Never loaded, or just dropped by EvictStalledTextures(). Load() ends up in
        // D3DX11CreateTextureFromMemory, which races the resource thread if run inline
        // here (this can run on the render thread mid-gameplay just as easily as on a
        // loading-screen thread during level load - neither is safe in the MT build).
        // Queue the (re)load the same way _CreateTexture() does for new textures instead;
        // bLoading guards against re-queuing every call while that's in flight.
        if (!flags.bLoading)
        {
#ifdef DEBUG
            Msg("* [TexReload] queued: %s", cName.c_str());
#endif // DEBUG
            CTexture* self = this;
            static DWORD this_thread_id = 0;
            this_thread_id = GetCurrentThreadId();
            textures_load_tasks.run([self]()
            {
                if (this_thread_id != GetCurrentThreadId()) { PROF_THREAD("X-Ray PPL Thread") }
                self->Load();
            });
        }
        // Bind nothing (or whatever is already there, e.g. mid-flight from an
        // in-flight Load()) for this call. apply_normal() waits out any in-flight
        // load, so this never touches D3D resources that are being created
        // concurrently. Once the queued Load() completes, PostLoad() re-points
        // `bind` at the real apply_* handler and subsequent calls stop landing here.
        apply_normal(dwStage);
        return;
    }

    PostLoad();
    if (bind == xr_make_delegate(this, &CTexture::apply_load))
    {
        // This should not happen - if bind is still apply_load, fall back to apply_normal
        // which will just apply the (potentially unloaded) surface
        apply_normal(dwStage);
    }
    else
    {
        bind(dwStage);
    }
};

void CTexture::ProcessStaging()
{
	VERIFY(pSurface);
	VERIFY(flags.bLoadedAsStaging);

	ID3DBaseTexture* pTargetSurface = 0;

	D3D_RESOURCE_DIMENSION type;
	pSurface->GetType(&type);

	switch (type)
	{
	case D3D_RESOURCE_DIMENSION_TEXTURE2D:
		{
			ID3DTexture2D* T = (ID3DTexture2D*)pSurface;
			D3D_TEXTURE2D_DESC TexDesc;
			T->GetDesc(&TexDesc);
			TexDesc.Usage = D3D_USAGE_DEFAULT;
			TexDesc.BindFlags = D3D_BIND_SHADER_RESOURCE;
			TexDesc.CPUAccessFlags = 0;

			T = 0;

			CHK_DX(HW.pDevice->CreateTexture2D( &TexDesc, // Texture desc
				NULL, // Initial data
				&T )); // [out] Texture

			pTargetSurface = T;
		}
		break;
	case D3D_RESOURCE_DIMENSION_TEXTURE3D:
		{
			ID3DTexture3D* T = (ID3DTexture3D*)pSurface;
			D3D_TEXTURE3D_DESC TexDesc;
			T->GetDesc(&TexDesc);
			TexDesc.Usage = D3D_USAGE_DEFAULT;
			TexDesc.BindFlags = D3D_BIND_SHADER_RESOURCE;
			TexDesc.CPUAccessFlags = 0;

			T = 0;

			CHK_DX(HW.pDevice->CreateTexture3D( &TexDesc, // Texture desc
				NULL, // Initial data
				&T )); // [out] Texture

			pTargetSurface = T;
		}
		break;
	default:
		VERIFY(!"CTexture::ProcessStaging unsupported dimensions.");
	}

	HW.pContext->CopyResource(pTargetSurface, pSurface);
	/*
	for( int i=0; i<iNumSubresources; ++i)
	{
		HW.pDevice->CopySubresourceRegion(
			pTargetSurface,
			i,
			0,
			0,
			0,
			pSurface,
			i,
			0
			);
	}
	*/


	flags.bLoadedAsStaging = FALSE;

	//	Check if texture was not copied _before_ it was converted.
	ULONG RefCnt = pSurface->Release();
	pSurface = 0;

	VERIFY(!RefCnt);

	surface_set(pTargetSurface);

	_RELEASE(pTargetSurface);
}

#ifdef USE_DX11
namespace tex_stream { void note_bound(CTexture* T); }
#endif

void CTexture::Apply(u32 dwStage)
{
#ifdef USE_DX11
	tex_stream::note_bound(this);
	m_last_used = Device.dwTimeContinual;
#endif
	if (flags.bLoading)
	{
		PROF_EVENT("CTexture: wait for its load");
		while (flags.bLoading)
			SwitchToThread();
	}
	dwLastUsedFrame = RDEVICE.dwFrame;

	if (flags.bLoadedAsStaging)
		ProcessStaging();
#ifdef USE_DX11
	if (m_stream == 1)
		stream_request();
#endif

	//if( !RImplementation.o.dx10_msaa )
	//   VERIFY( !((!pSurface)^(!m_pSRView)) );	//	Both present or both missing
	//else
	//{
	//if( ((!pSurface)^(!m_pSRView)) )
	//   return;
	//}

	if (dwStage < rstVertex) //	Pixel shader stage resources
	{
		//HW.pDevice->PSSetShaderResources(dwStage, 1, &m_pSRView);
		SRVSManager.SetPSResource(dwStage, m_pSRView);
	}
	else if (dwStage < rstGeometry) //	Vertex shader stage resources
	{
		//HW.pDevice->VSSetShaderResources(dwStage-rstVertex, 1, &m_pSRView);
		SRVSManager.SetVSResource(dwStage - rstVertex, m_pSRView);
	}
	else if (dwStage < rstHull) //	Geometry shader stage resources
	{
		//HW.pDevice->GSSetShaderResources(dwStage-rstGeometry, 1, &m_pSRView);
		SRVSManager.SetGSResource(dwStage - rstGeometry, m_pSRView);
	}
#ifdef USE_DX11
	else if (dwStage < rstDomain) //	Geometry shader stage resources
	{
		SRVSManager.SetHSResource(dwStage - rstHull, m_pSRView);
	}
	else if (dwStage < rstCompute) //	Geometry shader stage resources
	{
		SRVSManager.SetDSResource(dwStage - rstDomain, m_pSRView);
	}
	else if (dwStage < rstInvalid) //	Geometry shader stage resources
	{
		SRVSManager.SetCSResource(dwStage - rstCompute, m_pSRView);
	}
#endif
	else
		VERIFY("Invalid stage");
}

void CTexture::apply_theora(u32 dwStage)
{
	if (flags.bLoading)
	{
		PROF_EVENT("CTexture: wait for its load");
		while (flags.bLoading)
			SwitchToThread();
	}
	if (pTheora->Update(m_play_time != 0xFFFFFFFF ? m_play_time : Device.dwTimeContinual))
	{
		D3D_RESOURCE_DIMENSION type;
		pSurface->GetType(&type);
		R_ASSERT(D3D_RESOURCE_DIMENSION_TEXTURE2D == type);
		ID3DTexture2D* T2D = (ID3DTexture2D*)pSurface;
		D3D_MAPPED_TEXTURE2D mapData;
		RECT rect;
		rect.left = 0;
		rect.top = 0;
		rect.right = pTheora->Width(true);
		rect.bottom = pTheora->Height(true);

		u32 _w = pTheora->Width(false);

		//R_CHK				(T2D->LockRect(0,&R,&rect,0));
#ifdef USE_DX11
		R_CHK(HW.pContext->Map(T2D, 0, D3D_MAP_WRITE_DISCARD, 0, &mapData));
#else
		R_CHK(T2D->Map(0,D3D_MAP_WRITE_DISCARD,0,&mapData));
#endif
		//R_ASSERT			(R.Pitch == int(pTheora->Width(false)*4));
		R_ASSERT(mapData.RowPitch == int(pTheora->Width(false)*4));
		int _pos = 0;
		pTheora->DecompressFrame((u32*)mapData.pData, _w - rect.right, _pos);
		VERIFY(u32(_pos) == rect.bottom*_w);
		//R_CHK				(T2D->UnlockRect(0));
#ifdef USE_DX11
		HW.pContext->Unmap(T2D, 0);
#else
		T2D->Unmap(0);
#endif
	}
	Apply(dwStage);
	//CHK_DX(HW.pDevice->SetTexture(dwStage,pSurface));
};

void CTexture::apply_avi(u32 dwStage)
{
	if (flags.bLoading)
	{
		PROF_EVENT("CTexture: wait for its load");
		while (flags.bLoading)
			SwitchToThread();
	}
	if (pAVI->NeedUpdate())
	{
		D3D_RESOURCE_DIMENSION type;
		pSurface->GetType(&type);
		R_ASSERT(D3D_RESOURCE_DIMENSION_TEXTURE2D == type);
		ID3DTexture2D* T2D = (ID3DTexture2D*)pSurface;
		D3D_MAPPED_TEXTURE2D mapData;

		// AVI
		//R_CHK	(T2D->LockRect(0,&R,NULL,0));
#ifdef USE_DX11
		R_CHK(HW.pContext->Map(T2D, 0, D3D_MAP_WRITE_DISCARD, 0, &mapData));
#else
		R_CHK(T2D->Map(0,D3D_MAP_WRITE_DISCARD,0,&mapData));
#endif
		R_ASSERT(mapData.RowPitch == int(pAVI->m_dwWidth*4));
		BYTE* ptr;
		pAVI->GetFrame(&ptr);
		CopyMemory(mapData.pData, ptr, pAVI->m_dwWidth*pAVI->m_dwHeight*4);
		//R_CHK	(T2D->UnlockRect(0));
#ifdef USE_DX11
		HW.pContext->Unmap(T2D, 0);
#else
		T2D->Unmap(0);
#endif
	}
	//CHK_DX(HW.pDevice->SetTexture(dwStage,pSurface));
	Apply(dwStage);
};

void CTexture::apply_seq(u32 dwStage)
{
	if (flags.bLoading)
	{
		PROF_EVENT("CTexture: wait for its load");
		while (flags.bLoading)
			SwitchToThread();
	}
	// SEQ
	u32 frame = Device.dwTimeContinual / seqMSPF; //Device.dwTimeGlobal
	u32 frame_data = seqDATA.size();
	if (flags.seqCycles)
	{
		u32 frame_id = frame % (frame_data * 2);
		if (frame_id >= frame_data) frame_id = (frame_data - 1) - (frame_id % frame_data);
		pSurface = seqDATA[frame_id];
		m_pSRView = m_seqSRView[frame_id];
	}
	else
	{
		u32 frame_id = frame % frame_data;
		pSurface = seqDATA[frame_id];
		m_pSRView = m_seqSRView[frame_id];
	}
	//CHK_DX(HW.pDevice->SetTexture(dwStage,pSurface));
	Apply(dwStage);
};

void CTexture::apply_gif(u32 dwStage)
{
	while (flags.bLoading)
	{
		SwitchToThread();
	}
	if (gifPlayer->UpdateFrame())
	{
        const CGIFAnimationPlayer::Frame* const gifFrame = gifPlayer->GetActiveFrame();
        R_ASSERT(gifFrame);

        pSurface = gifFrame->surface;
        m_pSRView = gifFrame->srv;
	}
	Apply(dwStage);
}

void CTexture::apply_normal(u32 dwStage)
{
	if (flags.bLoading)
	{
		PROF_EVENT("CTexture: wait for its load");
		while (flags.bLoading)
			SwitchToThread();
	}
	//CHK_DX(HW.pDevice->SetTexture(dwStage,pSurface));
	Apply(dwStage);
};

void CTexture::Preload()
{
	if (!Core.ParamsData.test(ECoreParams::r4_dev))
	{
	m_bumpmap = DEV->m_textures_description.GetBumpName(cName);
	}
	m_material = DEV->m_textures_description.GetMaterial(cName);
}

void CTexture::Load()
{
	PROF_EVENT("CTexture::Load");

	if (flags.bLoaded || flags.bLoading) return;
	flags.bLoading = true;
	flags.bLoaded = false;
	desc_cache = 0;
	if (pSurface)
	{
		flags.bLoading = false;
		flags.bLoaded = true;
		return;
	}

	flags.bUser = false;
	flags.MemoryUsage = 0;
	if (0 == stricmp(*cName, "$null"))
	{
		flags.bLoading = false;
		flags.bLoaded = true;
		return;
	}
	if (0 != strstr(*cName, "$user$"))
	{
		flags.bUser = true;
		flags.bLoading = false;
		flags.bLoaded = true;
		return;
	}

	Preload();

	bool bCreateView = true;

	// Check for OGM
	string_path fn;
	if (FS.exist(fn, "$game_textures$", *cName, ".ogm"))
	{
		// AVI
		pTheora = xr_new<CTheoraSurface>();
		m_play_time = 0xFFFFFFFF;

		if (!pTheora->Load(fn))
		{
			xr_delete(pTheora);
			FATAL("Can't open video stream");
		}
		else
		{
			flags.MemoryUsage = pTheora->Width(true) * pTheora->Height(true) * 4;
			pTheora->Play(TRUE, Device.dwTimeContinual);

			// Now create texture
			ID3DTexture2D* pTexture = 0;
			u32 _w = pTheora->Width(false);
			u32 _h = pTheora->Height(false);

			//			HRESULT hrr = HW.pDevice->CreateTexture(
			//				_w, _h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &pTexture, NULL );
			D3D_TEXTURE2D_DESC desc;
			desc.Width = _w;
			desc.Height = _h;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.SampleDesc.Quality = 0;
			desc.Usage = D3D_USAGE_DYNAMIC;
			desc.BindFlags = D3D_BIND_SHADER_RESOURCE;
			desc.CPUAccessFlags = D3D_CPU_ACCESS_WRITE;
			desc.MiscFlags = 0;
			HRESULT hrr = HW.pDevice->CreateTexture2D(&desc, 0, &pTexture);

			pSurface = pTexture;
			if (FAILED(hrr))
			{
				FATAL("Invalid video stream");
				R_CHK(hrr);
				xr_delete(pTheora);
				pSurface = 0;
				m_pSRView = 0;
			}
			else
			{
				CHK_DX(HW.pDevice->CreateShaderResourceView(pSurface, 0, &m_pSRView));
			}
		}
	}
	else if (FS.exist(fn, "$game_textures$", *cName, ".avi"))
	{
		// AVI
		pAVI = xr_new<CAviPlayerCustom>();

		if (!pAVI->Load(fn))
		{
			xr_delete(pAVI);
			FATAL("Can't open video stream");
		}
		else
		{
			flags.MemoryUsage = pAVI->m_dwWidth * pAVI->m_dwHeight * 4;

			// Now create texture
			ID3DTexture2D* pTexture = 0;
			//HRESULT hrr = HW.pDevice->CreateTexture(
			//pAVI->m_dwWidth,pAVI->m_dwHeight,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,
			//	&pTexture,NULL
			//	);
			D3D_TEXTURE2D_DESC desc;
			desc.Width = pAVI->m_dwWidth;
			desc.Height = pAVI->m_dwHeight;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.SampleDesc.Quality = 0;
			desc.Usage = D3D_USAGE_DYNAMIC;
			desc.BindFlags = D3D_BIND_SHADER_RESOURCE;
			desc.CPUAccessFlags = D3D_CPU_ACCESS_WRITE;
			desc.MiscFlags = 0;
			HRESULT hrr = HW.pDevice->CreateTexture2D(&desc, 0, &pTexture);

			pSurface = pTexture;
			if (FAILED(hrr))
			{
				FATAL("Invalid video stream");
				R_CHK(hrr);
				xr_delete(pAVI);
				pSurface = 0;
				m_pSRView = 0;
			}
			else
			{
				CHK_DX(HW.pDevice->CreateShaderResourceView(pSurface, 0, &m_pSRView));
			}
		}
	}
	else if (FS.exist(fn, "$game_textures$", *cName, ".seq"))
	{
		// Sequence
		string256 buffer;
		IReader* _fs = FS.r_open(fn);

		flags.seqCycles = FALSE;
		_fs->r_string(buffer, sizeof(buffer));
		if (0 == stricmp(buffer, "cycled"))
		{
			flags.seqCycles = TRUE;
			_fs->r_string(buffer, sizeof(buffer));
		}
		u32 fps = atoi(buffer);
		seqMSPF = 1000 / fps;

		// Resolved name of each seqDATA entry. A frame file that repeats is loaded once:
		// later entries share its surface and view, each holding its own reference, so
		// Unload() still releases every entry once. MemoryUsage counts it once.
		xr_vector<shared_str> seq_names;
		while (!_fs->eof())
		{
			_fs->r_string(buffer, sizeof(buffer));
			_Trim(buffer);
			if (buffer[0])
			{
				string_path resolved;
				xr_strcpy(resolved, buffer);
				fix_texture_name(resolved);
				xr_strlwr(resolved);
				const shared_str name = resolved;

				const auto it = std::find(seq_names.begin(), seq_names.end(), name);
				if (it != seq_names.end())
				{
					const size_t first = it - seq_names.begin();
					seqDATA.push_back(seqDATA[first]);
					seqDATA.back()->AddRef();
					m_seqSRView.push_back(m_seqSRView[first]);
					if (m_seqSRView.back())
						m_seqSRView.back()->AddRef();
					seq_names.push_back(name);
					continue;
				}

				// Load another texture
				u32 mem = 0;
				pSurface = ::RImplementation.texture_load(buffer, mem);
				if (pSurface)
				{
#ifdef USE_DX11
					gpu_memory::track(pSurface, buffer);
#endif
					// pSurface->SetPriority	(PRIORITY_LOW);
					seqDATA.push_back(pSurface);
					m_seqSRView.push_back(0);
					HW.pDevice->CreateShaderResourceView(seqDATA.back(), NULL, &m_seqSRView.back());
					flags.MemoryUsage += mem;
					seq_names.push_back(name);
				}
			}
		}
		pSurface = 0;
		FS.r_close(_fs);
	}
    else if (FS.exist(fn, "$game_textures$", *cName, ".gif"))
    {
        gifPlayer = xr_new<CGIFAnimationPlayer>();
        if (!gifPlayer->Load(fn))
        {
            xr_delete(gifPlayer);
            pSurface = nullptr;
            m_pSRView = nullptr;
        }
        else
        {
            flags.MemoryUsage = gifPlayer->GetUsedMemory();

            gifPlayer->Play();

            const CGIFAnimationPlayer::Frame* const gifFrame = gifPlayer->GetActiveFrame();
            pSurface = gifFrame->surface;
            m_pSRView = gifFrame->srv;
        }
    }
	else
	{
		// Normal texture
		u32 mem = 0;
		//pSurface = ::RImplementation.texture_load	(*cName,mem);
#ifdef USE_DX11
		const bool stream = m_stream_eligible && ps_r4_tex_stream;
		g_texture_load_max_dim = stream ? tex_streaming::kSmallSize : 0;
#endif
		pSurface = ::RImplementation.texture_load(*cName, mem, true);
#ifdef USE_DX11
		g_texture_load_max_dim = 0;
		m_mip_skip = u8(g_texture_load_skip);
		// Streamed when the small version is smaller than the texture would be (else it is small anyway)
		m_stream = (stream && pSurface && g_texture_load_skip > g_texture_load_lod_normal) ? 1 : 0;
#endif

		if (GetUsage() == D3D_USAGE_STAGING)
		{
			flags.bLoadedAsStaging = TRUE;
			bCreateView = false;
		}

		// Calc memory usage and preload into vid-mem
		if (pSurface)
		{
			// pSurface->SetPriority	(PRIORITY_NORMAL);
			flags.MemoryUsage = mem;
#ifdef USE_DX11
			gpu_memory::track(pSurface, *cName);
#endif
		}
		
		if (pSurface && bCreateView)
			CHK_DX(HW.pDevice->CreateShaderResourceView(pSurface, NULL, &m_pSRView));
	}
	PostLoad();
	flags.bLoading = false;
	flags.bLoaded = true;
}

void CTexture::Unload()
{
	if (flags.bLoading)
	{
		PROF_EVENT("CTexture: wait for its load");
		while (flags.bLoading)
			SwitchToThread();
	}
#ifdef USE_DX11
	stream_drop();
#endif

	// Already unloaded or never loaded: nothing to do.
	if (!flags.bLoaded)
		return;
#ifdef DEBUG
	string_path				msg_buff;
	xr_sprintf				(msg_buff,sizeof(msg_buff),"* Unloading texture [%s] pSurface RefCount=",cName.c_str());
#endif // DEBUG

	//.	if (flags.bLoaded)		Msg		("* Unloaded: %s",cName.c_str());

	flags.bLoaded = FALSE;
	flags.bLoadedAsStaging = FALSE;
	if (!seqDATA.empty())
	{
		for (u32 I = 0; I < seqDATA.size(); I++)
		{
			_RELEASE(seqDATA[I]);
			_RELEASE(m_seqSRView[I]);
		}
		seqDATA.clear();
		m_seqSRView.clear();
		pSurface = 0;
		m_pSRView = 0;
	}

    if (gifPlayer)
    {
        xr_delete(gifPlayer);
        pSurface = nullptr;
        m_pSRView = nullptr;
    }

#ifdef DEBUG
	_SHOW_REF		(msg_buff, pSurface);
#endif // DEBUG
	_RELEASE(pSurface);
	_RELEASE(m_pSRView);

	xr_delete(pAVI);
	xr_delete(pTheora);

	bind = xr_make_delegate(this, &CTexture::apply_load);
}

void CTexture::desc_update()
{
	if (flags.bLoading)
	{
		PROF_EVENT("CTexture: wait for its load");
		while (flags.bLoading)
			SwitchToThread();
	}
	desc_cache = pSurface;
	if (pSurface)
	{
		D3D_RESOURCE_DIMENSION type;
		pSurface->GetType(&type);
		if (D3D_RESOURCE_DIMENSION_TEXTURE2D == type)
		{
			ID3DTexture2D* T = (ID3DTexture2D*)pSurface;
			T->GetDesc(&desc);
		}
	}
}

D3D_USAGE CTexture::GetUsage()
{
	D3D_USAGE res = D3D_USAGE_DEFAULT;

	if (pSurface)
	{
		D3D_RESOURCE_DIMENSION type;
		pSurface->GetType(&type);
		switch (type)
		{
		case D3D_RESOURCE_DIMENSION_TEXTURE1D:
			{
				ID3DTexture1D* T = (ID3DTexture1D*)pSurface;
				D3D_TEXTURE1D_DESC descr;
				T->GetDesc(&descr);
				res = descr.Usage;
			}
			break;

		case D3D_RESOURCE_DIMENSION_TEXTURE2D:
			{
				ID3DTexture2D* T = (ID3DTexture2D*)pSurface;
				D3D_TEXTURE2D_DESC descr;
				T->GetDesc(&descr);
				res = descr.Usage;
			}
			break;

		case D3D_RESOURCE_DIMENSION_TEXTURE3D:
			{
				ID3DTexture3D* T = (ID3DTexture3D*)pSurface;
				D3D_TEXTURE3D_DESC descr;
				T->GetDesc(&descr);
				res = descr.Usage;
			}
			break;

		default:
			VERIFY(!"Unknown texture format???");
		}
	}

	return res;
}

void CTexture::video_Play(BOOL looped, u32 _time)
{
	if (flags.bLoading)
	{
		PROF_EVENT("CTexture: wait for its load");
		while (flags.bLoading)
			SwitchToThread();
	}
	if (pTheora) pTheora->Play(looped, (_time != 0xFFFFFFFF) ? (m_play_time = _time) : Device.dwTimeContinual);
}

void CTexture::video_Pause(BOOL state)
{
	if (flags.bLoading)
	{
		PROF_EVENT("CTexture: wait for its load");
		while (flags.bLoading)
			SwitchToThread();
	}
	if (pTheora) pTheora->Pause(state);
}

void CTexture::video_Stop()
{
	if (flags.bLoading)
	{
		PROF_EVENT("CTexture: wait for its load");
		while (flags.bLoading)
			SwitchToThread();
	}
	if (pTheora) pTheora->Stop();
}

BOOL CTexture::video_IsPlaying()
{
	if (flags.bLoading)
	{
		PROF_EVENT("CTexture: wait for its load");
		while (flags.bLoading)
			SwitchToThread();
	}
	return (pTheora) ? pTheora->IsPlaying() : FALSE;
}
