#ifndef __X_RAY_H__
#define __X_RAY_H__

// refs
class ENGINE_API CGameFont;

#include "../Include/xrRender/FactoryPtr.h"
#include "../Include/xrRender/ApplicationRender.h"
#include <functional>

// definition
class ENGINE_API CApplication :
	public pureFrame,
	public IEventReceiver
{
	friend class dxApplicationRender;
	friend class CScreenFade;

	// levels
	struct sLevelInfo
	{
		char* folder;
		char* name;
	};

public:
	string2048 ls_header;
	string2048 ls_tip_number;
	string2048 ls_tip;
private:
	FactoryPtr<IApplicationRender> m_pRender;
	string_path m_loading_logo_level{};
	string_path m_loading_logo{};

	int max_load_stage;

	int load_stage;

	u32 ll_dwReference;
private:
	EVENT eQuit;
	EVENT eStart;
	EVENT eStartLoad;
	EVENT eDisconnect;
	EVENT eConsole;
	EVENT eStartMPDemo;

	void Level_Append(LPCSTR lname);
public:
	CGameFont* pFontSystem;

	// Levels
	xr_vector<sLevelInfo> Levels;
	u32 Level_Current;
	void Level_Scan();
	int Level_ID(LPCSTR name, LPCSTR ver, bool bSet);
	void Level_Set(u32 ID);
	void LoadAllArchives();
	CInifile* GetArchiveHeader(LPCSTR name, LPCSTR ver);

	// Loading
	void LoadBegin();
	void LoadScreenSetLevelLogo(LPCSTR level_name);
	void LoadEnd();
	void LoadTitleInt(LPCSTR str1, LPCSTR str2, LPCSTR str3);
	void LoadStage();
	void LoadSwitch();
	void LoadDraw();

	virtual void OnEvent(EVENT E, u64 P1, u64 P2);

	// Other
	CApplication();
	virtual ~CApplication();

	virtual void _BCL OnFrame();
	void load_draw_internal();
	void destroy_loading_shaders();
};

extern ENGINE_API CApplication* pApp;
extern ENGINE_API int psLoadingScreenEarly;

// Level changes behind a black screen (fade_transitions): the screen fades to black before a level changer travels,
// the load and its precache frames show black instead of the loading screen, no "press any key" (its script callbacks
// still run), then the new level fades in. Loads of saved games keep the loading screen
extern ENGINE_API int g_fade_transitions;
class ENGINE_API CScreenFade : public pureRender, public pureFrame
{
public:
	// Fades to black over ms, then calls on_black once (a level changer sends its travel request) and stays black
	void fade_out(u32 ms, std::function<void()> on_black);
	// A level change starts loading: black until the precache frames end, then the new level fades in
	void hold_for_load();
	// A load of a saved game: back to normal
	void cancel();
	// CApplication::LoadBegin: the load of the level change has started
	void on_load_begin() { if (m_state == sLoad) m_load_started = true; }
	bool loading_black() const { return m_state == sLoad; }
	bool busy() const { return m_state != sNone; }
	virtual void OnRender();
	virtual void OnFrame(); // on_black runs here, not while rendering

private:
	enum State { sNone, sOut, sBlack, sLoad, sIn };
	State m_state = sNone;
	u64 m_start = 0; // CPU::QPC of the state's start
	u32 m_ms = 0;
	std::function<void()> m_on_black;
	bool m_black_reached = false;
	bool m_load_started = false; // sLoad: the old level still renders a few frames before its load begins
	bool m_precache_seen = false; // sLoad: the new level's precache frames have started
	float elapsed_ms() const;
};
extern ENGINE_API CScreenFade g_screen_fade;

//Discord
struct rpc_info
{
	bool mainmenu;
	bool loadscreen;
	bool ingame;
	bool ex_update;
	bool ironman;
	bool godmode;
	int possessed_lives;
	int health;
	int lives_left;
	int level_icon_index;
	char task_name[128];
	char faction_name[128];
	char rank_name[128];
	char reputation[128];
	char level_name[128];
	char gamemode[128];
	LPCSTR currenttime;
	LPCSTR faction;
	LPCSTR level;
};

struct rpc_strings
{
	char loading[128];
	char mainmenu[128];
	char paused[128];
	char health[128];
	char livesleft[128];
	char dead[128];
	char livesleftsingle[128];
	char livespossessed[128];
	char livespossessedsingle[128];
	char godmode[128];
};

extern ENGINE_API void updateDiscordPresence();
extern ENGINE_API rpc_info discord_gameinfo;
extern ENGINE_API rpc_strings discord_strings;
extern ENGINE_API float discord_update_rate;

LPCSTR xr_ToUTF8(LPCSTR input, int max_length = 128);

void clearDiscordPresence();

#endif //__XR_BASE_H__
