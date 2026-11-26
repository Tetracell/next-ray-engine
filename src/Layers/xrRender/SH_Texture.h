#ifndef SH_TEXTURE_H
#define SH_TEXTURE_H
#pragma once

#include "../../xrCore/xr_resource.h"
#include <atomic>

class ENGINE_API CAviPlayerCustom;
class CTheoraSurface;
class CGIFAnimationPlayer;

class ECORE_API CTexture : public xr_resource_named
{
public:
	//	Since DX10 allows up to 128 unique textures, 
	//	distance between enum values should be at leas 128
	enum ResourceShaderType //	Don't change this since it's hardware-dependent
	{
		rstPixel = 0,
		//	Default texture offset
		rstVertex = D3DVERTEXTEXTURESAMPLER0,
		rstGeometry = rstVertex + 256,
		rstHull = rstGeometry + 256,
		rstDomain = rstHull + 256,
		rstCompute = rstDomain + 256,
		rstInvalid = rstCompute + 256
	};

public:
	void __stdcall apply_load(u32 stage);
	void __stdcall apply_theora(u32 stage);
	void __stdcall apply_avi(u32 stage);
	void __stdcall apply_seq(u32 stage);
    void __stdcall apply_gif(u32 stage);
	void __stdcall apply_normal(u32 stage);

	void Preload();
	void Load();
	void PostLoad();
	void Unload(void);
	//	void								Apply			(u32 dwStage);

	void surface_set(ID3DBaseTexture* surf);
	ID3DBaseTexture* surface_get();

	IC BOOL isUser() { return flags.bUser; }
	// The size of the whole texture: a streamed one is loaded whole for good first (callers may rely on it)
	IC u32 get_Width()
	{
#ifdef USE_DX11
		if (m_stream) stream_whole_now();
#endif
		desc_enshure();
		return desc.Width;
	}

	IC u32 get_Height()
	{
#ifdef USE_DX11
		if (m_stream) stream_whole_now();
#endif
		desc_enshure();
		return desc.Height;
	}

	// The size of what is loaded now (a streamed texture's small version too), for measuring
	IC u32 get_Width_loaded()
	{
		desc_enshure();
		return desc.Width;
	}

	IC u32 get_Height_loaded()
	{
		desc_enshure();
		return desc.Height;
	}

#ifdef USE_DX11
	// Texture streaming (r4_tex_stream), see dx10SH_Texture.cpp
	void stream_request();
	void stream_swap_in();
	void stream_evict();
	void stream_whole_now();
	void stream_drop();
#endif

	void video_Sync(u32 _time) { m_play_time = _time; }
	void video_Play(BOOL looped, u32 _time = 0xFFFFFFFF);
	void video_Pause(BOOL state);
	void video_Stop();
	BOOL video_IsPlaying();

	CTexture();
	virtual ~CTexture();

#if defined(USE_DX10) || defined(USE_DX11)
	ID3DShaderResourceView*				get_SRView() {return m_pSRView;}
	// The view the texture binds, without ownership: a kept lamp shadow map in place of the atlas (light_smap_keep).
	// The caller restores the texture's own view afterwards
	void								set_SRView(ID3DShaderResourceView* v) { m_pSRView = v; }
	void								apply_view(u32 stage) { Apply(stage); } // bind the current view on that stage
	// Exchanges the resources and views of two render target textures of the same size and format (the post-process
	// ping-pong, CRenderTarget::post_result); the backend's binding cache of both is cleared by the caller
	void								surface_swap(CTexture& other)
	{
		wait_for_loading();
		other.wait_for_loading();
		std::swap(pSurface, other.pSurface);
		std::swap(m_pSRView, other.m_pSRView);
		std::swap(desc_cache, other.desc_cache);
		std::swap(desc, other.desc);
	}
#endif	//	USE_DX10

private:
	IC void wait_for_loading() const { while (flags.bLoading){SwitchToThread();} }
	IC BOOL desc_valid() { wait_for_loading(); return pSurface==desc_cache; }
	IC void desc_enshure() { wait_for_loading(); if (!desc_valid()) desc_update(); }
	void desc_update();
#if defined(USE_DX10) || defined(USE_DX11)
	void								Apply			(u32 dwStage);
	void								ProcessStaging();
	D3D_USAGE							GetUsage();
#endif	//	USE_DX10

	//	Class data
public: //	Public class members (must be encapsulated furthur)
	struct
	{
		u32 bLoaded : 1;
		u32 bLoading : 1;
		u32 bUser : 1;
		u32 seqCycles : 1;
		u32 MemoryUsage : 27;
#if defined(USE_DX10) || defined(USE_DX11)
		u32					bLoadedAsStaging: 1;
#endif	//	USE_DX10
	} flags;

    u32 dwLastUsedFrame = 0; // frame index of last Apply() call — used for eviction
    // Set by owners that copy the surface instead of applying it, eviction skips these
    bool bNoEvict = false;

	xr_delegate<void(u32)> bind;


	CAviPlayerCustom* pAVI;
	CTheoraSurface* pTheora;
	float m_material;
	shared_str m_bumpmap;

	bool m_is_hot = false;		//--DSR-- HeatVision
	bool m_is_glowing = false;	//--DSR-- SilencerOverheat
	u8 m_mip_skip = 0; // mip levels dropped when loading (texture_lod)
	bool m_on_demand = false; // not loaded with the level, only when used (weather sky textures, r4_sky_on_demand)
	u32 m_last_used = 0; // Device.dwTimeContinual of the last bind
#ifdef USE_DX11
	// Texture streaming (r4_tex_stream): loaded small with the level, whole on its first bind
	bool m_stream_eligible = false; // a world texture (set when created)
	u8 m_stream = 0; // 0 not streamed, 1 small, 2 whole one loading, 3 whole
	u8 m_stream_small_skip = 0;
	u8 m_stream_pending_skip = 0;
	u32 m_stream_small_mem = 0;
	u32 m_stream_pending_mem = 0;
	ID3DBaseTexture* m_stream_small = nullptr; // the small version, kept while the whole one is used
	ID3DBaseTexture* m_stream_pending = nullptr; // the whole one, loaded in the background
	std::atomic<bool> m_stream_done{ false }; // the background load finished (m_stream_pending set, may be null)
#endif

	union
	{
		u32 m_play_time; // sync theora time
		u32 seqMSPF; // Sequence data milliseconds per frame
	};

private:
	ID3DBaseTexture* pSurface;
	// Sequence data
	xr_vector<ID3DBaseTexture*> seqDATA;

	// Description
	ID3DBaseTexture* desc_cache;
	D3D_TEXTURE2D_DESC desc;

    CGIFAnimationPlayer* gifPlayer;

#if defined(USE_DX10) || defined(USE_DX11)
	ID3DShaderResourceView*			m_pSRView;
	// Sequence view data
	xr_vector<ID3DShaderResourceView*>m_seqSRView;
#endif	//	USE_DX10
};

struct resptrcode_texture : public resptr_base<CTexture>
{
	void create(LPCSTR _name);
	void destroy() { _set(NULL); }
	// The bump map's name is set in Preload, before the texture loads (on a worker thread): no wait for the load, which
	// made every shader built on a texture not loaded yet wait for its file (~3 s of a level load on macOS)
	shared_str bump_get() { return _get()->m_bumpmap; }
	bool bump_exist() { return 0!=bump_get().size(); }
};

typedef resptr_core<CTexture, resptrcode_texture>
ref_texture;

#endif
