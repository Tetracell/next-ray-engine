#include "stdafx.h"
#include "r4_hiz.h"
#include "r4_tex_stream.h"
#include "../../xrEngine/igame_persistent.h"
#include "../xrRender/FBasicVisual.h"
#include "../../xrEngine/customhud.h"
#include "../../xrEngine/xr_object.h"
#include "../xrRender/SkeletonCustom.h"
#include "../../xrParticles/ParticlesAsyncManager.h"

#include "../xrRender/QueryHelper.h"

#include	"../xrRender/dxRenderDeviceRender.h"
namespace
{
bool ui_3d_icon_atlas_active = false;
Fmatrix ui_3d_icon_project_saved;
Fmatrix ui_3d_icon_full_transform_saved;
}


void CRender::render_menu()
{
	PIX_EVENT(render_menu);
	//	Globals
	RCache.set_CullMode(CULL_CCW);
	RCache.set_Stencil(FALSE);
	RCache.set_ColorWriteEnable();

	// Upscaler: rt_Generic_0/1 are at render resolution, draw the menu UI straight to the
	// output (without the distortion effect) instead of squeezing it into the smaller targets
	if (Target->upscale_enabled())
	{
		Target->u_setrt(Device.dwRenderWidth, Device.dwRenderHeight, HW.pBaseRT, NULL, NULL, HW.pBaseZB);
		rmNormal();
		g_pGamePersistent->OnRenderPPUI_main(); // PP-UI
		return;
	}

	// Main Render
	{
		Target->u_setrt(Target->rt_Generic_0, 0, 0, HW.pBaseZB); // LDR RT
		g_pGamePersistent->OnRenderPPUI_main(); // PP-UI
	}

	// Distort
	{
		FLOAT ColorRGBA[4] = {127.0f / 255.0f, 127.0f / 255.0f, 0.0f, 127.0f / 255.0f};
		Target->u_setrt(Target->rt_Generic_1, 0, 0, HW.pBaseZB); // Now RT is a distortion mask
		HW.pContext->ClearRenderTargetView(Target->rt_Generic_1->pRT, ColorRGBA);
		g_pGamePersistent->OnRenderPPUI_PP(); // PP-UI
	}

	// Actual Display
	Target->u_setrt(Device.dwRenderWidth, Device.dwRenderHeight, HW.pBaseRT,NULL,NULL, HW.pBaseZB);
	RCache.set_Shader(Target->s_menu);
	RCache.set_Geometry(Target->g_menu);

	Fvector2 p0, p1;
	u32 Offset;
	auto C = color_rgba(255, 255, 255, 255);
	float _w = float(Device.dwRenderWidth);
	float _h = float(Device.dwRenderHeight);
	float d_Z = EPS_S;
	float d_W = 1.f;
	p0.set(.5f / _w, .5f / _h);
	p1.set((_w + .5f) / _w, (_h + .5f) / _h);

	FVF::TL* pv = (FVF::TL*)RCache.Vertex.Lock(4, Target->g_menu->vb_stride, Offset);
	pv->set(EPS, float(_h + EPS), d_Z, d_W, C, p0.x, p1.y);
	pv++;
	pv->set(EPS, EPS, d_Z, d_W, C, p0.x, p0.y);
	pv++;
	pv->set(float(_w + EPS), float(_h + EPS), d_Z, d_W, C, p1.x, p1.y);
	pv++;
	pv->set(float(_w + EPS), EPS, d_Z, d_W, C, p1.x, p0.y);
	pv++;
	RCache.Vertex.Unlock(4, Target->g_menu->vb_stride);
	RCache.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);
}

extern u32 g_r;

// r4_cull_freeze 1: the main view keeps culling (frustum, sectors, occlusion, screen size) from where the camera was when
// it was turned on, while the frame is drawn from wherever the camera is now: fly out and see what is drawn. Not saved
int ps_r4_cull_freeze = 0;
extern const Fvector* r_cull_view_pos;
namespace
{
	bool s_freeze_have = false;
	Fmatrix s_freeze_full;
	Fvector s_freeze_pos;
	CSector* s_freeze_sector = nullptr;
}

// The item and camera attachment UI was drawn this frame in render_forward (no upscaler)
static bool s_hud_ui_drawn = false;

void CRender::Render()
{
	RCache.constants_epoch++; // per-frame constant handlers run once per render (R_constant_setup::per_frame)
	PIX_EVENT_C(CRender_Render, dx10_marker_frame);
	dx10_annotate_frame();
	s_hud_ui_drawn = false;

	rmNormal();

	bool _menu_pp = g_pGamePersistent ? g_pGamePersistent->OnRenderPPUI_query() : false;
	if (_menu_pp)
	{
		render_menu();
		return;
	};

	IMainMenu* pMainMenu = g_pGamePersistent ? g_pGamePersistent->m_pMainMenu : 0;
	bool bMenu = pMainMenu ? pMainMenu->CanSkipSceneRendering() : false;

	if (!(g_pGameLevel && g_hud)
		|| bMenu)
	{
		Target->u_setrt(Device.dwRenderWidth, Device.dwRenderHeight, HW.pBaseRT,NULL,NULL, HW.pBaseZB);
		return;
	}

	if (m_bFirstFrameAfterReset)
	{
		for (light* L : v_all_lights)//critical!!!
			L->m_moving_frames = 0;

		m_bFirstFrameAfterReset = false;
		return;
	}

	// Upscaler: render the scene at the internal resolution until Target->phase_upscale() upscales it.
	// The guard restores the output resolution if the frame leaves early.
	Target->upscale_begin_scene();
	struct upscale_scene_guard
	{
		CRenderTarget* T;
		~upscale_scene_guard() { T->upscale_end_scene(); }
	} upscale_guard = { Target };

	if ((Device.dwFrame % (u32)ps_r__tex_evict_interval) == 0)
		dxRenderDeviceRender::Instance().Resources->EvictStalledTextures();

	//.	VERIFY					(g_pGameLevel && g_pGameLevel->pHUD);

	// Configure
	RImplementation.o.distortion = FALSE; // disable distorion
	Fcolor sun_color = ((light*)Lights.sun_adapted._get())->color;
	BOOL bSUN = ps_r2_ls_flags.test(R2FLAG_SUN) && (u_diffuse2s(sun_color.r, sun_color.g, sun_color.b)>EPS) && !Core.ParamsData.test(ECoreParams::r4_dev);
	if (o.sunstatic) bSUN = FALSE;
	// Msg						("sstatic: %s, sun: %s",o.sunstatic?;"true":"false", bSUN?"true":"false");

	// The culling view: the camera's, or the frozen one (r4_cull_freeze)
	const bool freeze = ps_r4_cull_freeze && !Device.m_SecondViewport.IsSVPFrame() && RImplementation.pLastSector;
	if (freeze && !s_freeze_have)
	{
		s_freeze_have = true;
		s_freeze_full.set(Device.mFullTransform);
		s_freeze_pos.set(Device.vCameraPosition);
		s_freeze_sector = RImplementation.pLastSector;
	}
	if (!ps_r4_cull_freeze)
		s_freeze_have = false;
	hiz::set_frozen(s_freeze_have);
	r_cull_view_pos = freeze ? &s_freeze_pos : nullptr;
	Fmatrix cull_full = freeze ? s_freeze_full : Device.mFullTransform;
	Fvector cull_pos = freeze ? s_freeze_pos : Device.vCameraPosition;
	CSector* cull_sector = freeze ? s_freeze_sector : RImplementation.pLastSector;

	// HOM
	ViewBase.CreateFromMatrix(cull_full, FRUSTUM_P_LRTB + FRUSTUM_P_FAR);
	if (!freeze)
		HOM.MT_RENDER();

	Target->phase_scene_prepare();


	//******* Main calc - DEFERRER RENDERER
	phase = PHASE_NORMAL;
	
	/*if (RImplementation.o.ssfx_core) // SSS23: DEPRECATED
	{
		// HUD Masking rendering
		FLOAT ColorRGBA[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
		HW.pContext->ClearRenderTargetView(Target->rt_ssfx_hud->pRT, ColorRGBA);

		Target->u_setrt(Target->rt_ssfx_hud, NULL, NULL, HW.pBaseZB);
		r_dsgraph_render_hud(true);

		// Reset Depth
		HW.pContext->ClearDepthStencilView(HW.pBaseZB, D3D_CLEAR_DEPTH, 1.0f, 0);
	}*/

    // Occlusion culling: the 3D scope's view isn't tested against the main view's depth
    const bool hiz_frame = !Device.m_SecondViewport.IsSVPFrame() && !RImplementation.o.dx10_msaa;
    hiz::frame_begin(hiz_frame);
    if (hiz_frame)
        tex_stream::frame_begin();
    GMBase.traverse(cull_sector, ViewBase, cull_pos, cull_full);
    GMBase.r_dsgraph_capture_static();
    GMBase.r_dsgraph_capture_dynamic();

    if (RImplementation.o.ssfx_motionvectors)
    {
        PIX_EVENT(SSFX_MOTION_VECTORS);
        Target->u_setrt(Device.dwRenderWidth, Device.dwRenderHeight, 0, 0, Target->rt_ssfx_motion_vectors->pRT, 0);

        FLOAT ColorRGBA[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        HW.pContext->ClearRenderTargetView(Target->rt_ssfx_motion_vectors->pRT, ColorRGBA);

        RCache.set_Stencil(FALSE);
        g_pGamePersistent->Environment().RenderSky(true);

        RCache.Index.Flush();
        RCache.Vertex.Flush();

        RCache.set_xform_world(Fidentity);
    }

	if (ps_r2_ls_flags.test(R2FLAG_TERRAIN_PREPASS))
	{
		Target->u_setrt(Device.dwRenderWidth, Device.dwRenderHeight, NULL, NULL, NULL, !RImplementation.o.dx10_msaa ? HW.pBaseZB : Target->rt_MSAADepth->pZRT);
	}

	//******* Main render :: PART-0	-- first
	{
		PIX_EVENT_C(DEFER_PART0_SPLIT, dx10_marker_gbuffer);
		// level, SPLIT
		Target->phase_scene_begin();
		{ PIX_EVENT(STATIC_GEOMETRY); GMBase.r_dsgraph_render_static(0); }
		{ PIX_EVENT(DYNAMIC_OBJECTS); GMBase.r_dsgraph_render_dynamic(0); }
		Target->disable_aniso();
		// Scene depth without HUD, for the occlusion culling of a later frame
		if (hiz_frame)
			hiz::capture(HW.pBaseZB, Device.mFullTransform);
	}

	//  Redotix99: for 3D Shader Based Scopes 	
	if (scope_3D_fake_enabled)
	{
		ID3DResource* zbuffer_res;
		HW.pBaseZB->GetResource(&zbuffer_res);
		HW.pContext->CopyResource(RImplementation.Target->rt_tempzb->pSurface, zbuffer_res);
#ifdef USE_DX12
		_RELEASE(zbuffer_res);
#endif
	}

	if (RImplementation.o.dx10_msaa)
		RCache.set_ZB(RImplementation.Target->rt_MSAADepth->pZRT);

	{
		PIX_EVENT(DEFER_TEST_LIGHT_VIS);
		//******* Occlusion testing of volume-limited light-sources
		Target->phase_occq();
		LP_normal.clear();
		LP_pending.clear();
		GMBase.r_dsgraph_capture_lights();
	}

	//******* Main render :: PART-1 (second)
	{
		PIX_EVENT_C(DEFER_PART1_SPLIT, dx10_marker_gbuffer);
		// level
		Target->phase_scene_begin();
		GMBase.r_dsgraph_capture_hud();
		{ PIX_EVENT(HUD_WEAPON); GMBase.r_dsgraph_render_hud(); }
		{ PIX_EVENT(LODS); GMBase.r_dsgraph_render_lods(true,true); }
		if (Details) { PIX_EVENT(GRASS); Details->Render(); }
		Target->phase_scene_end();
	}

	// Wall marks
	if (Wallmarks)
	{
		PIX_EVENT(DEFER_WALLMARKS);
		Target->phase_wallmarks();

		Wallmarks->Render(); // wallmarks has priority as normal geometry
	}

	// full screen pass to mark msaa-edge pixels in highest stencil bit
	if (RImplementation.o.dx10_msaa)
	{
		PIX_EVENT(MARK_MSAA_EDGES);
		Target->mark_msaa_edges();
	}

	//	TODO: DX10: Implement DX10 rain.
	if (ps_r2_ls_flags.test(R3FLAG_DYN_WET_SURF))
	{
		PIX_EVENT(DEFER_RAIN);
		render_rain();
	}

	{
		// Save previus and current matrices
		{
			static Fmatrix mm_saved_viewproj;

			if (!Device.m_SecondViewport.IsSVPFrame())
			{
				Target->Matrix_previous.mul(mm_saved_viewproj, Device.mInvView);
				Target->Matrix_current.set(Device.mProject);
				mm_saved_viewproj.set(Device.mFullTransform);
			}
		}

		if (RImplementation.o.ssfx_sss && !Device.m_SecondViewport.IsSVPFrame())
		{
			static bool sss_rendered, sss_extended_rendered;

			// SSS Shadows
			if (ps_ssfx_sss_quality.z > 0)
			{
				PIX_EVENT(SSFX_SSS_SHADOWS);
				Target->phase_ssfx_sss();
				sss_rendered = true;
			}
			else
			{
				if (sss_rendered) // Clear buffer
				{
					sss_rendered = false;
					FLOAT ColorRGBA[4] = { 1,1,1,1 };
					HW.pContext->ClearRenderTargetView(Target->rt_ssfx_sss->pRT, ColorRGBA);
				}
			}

			if (ps_ssfx_sss_quality.w > 0)
			{
				// Extra lights
				PIX_EVENT(SSFX_SSS_EXTRA_LIGHTS);
				Target->phase_ssfx_sss_ext(RImplementation.LP_normal);
				sss_extended_rendered = true;
			}
			else
			{
				if (sss_extended_rendered) // Clear buffer
				{
					sss_extended_rendered = false;
					FLOAT ColorRGBA[4] = { 1,1,1,1 };
					HW.pContext->ClearRenderTargetView(Target->rt_ssfx_sss_tmp->pRT, ColorRGBA);
				}
			}
		}
	}

	// Directional light - fucking sun
	if (bSUN) //bSUN && Device.dwFrame & 1 --Delayed sun update. Worth to check it in future
	{
		PIX_EVENT_C(DEFER_SUN, dx10_marker_lights);
		RImplementation.stats.l_visible ++;
		render_sun_cascades();
		Target->increment_light_marker();
		Target->accum_direct_blend();
	}

	phase = PHASE_NORMAL;

	{
		PIX_EVENT_C(DEFER_SELF_ILLUM, dx10_marker_lights);
		Target->phase_accumulator();
		// Render emissive geometry, stencil - write 0x0 at pixel pos
		RCache.set_xform_project(Device.mProject);
		RCache.set_xform_view(Device.mView);
		// Stencil - write 0x1 at pixel pos - 
		if (!RImplementation.o.dx10_msaa)
			RCache.set_Stencil(TRUE, D3DCMP_ALWAYS, 0x01, 0xff, 0xff, D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE,
			                   D3DSTENCILOP_KEEP);
		else
			RCache.set_Stencil(TRUE, D3DCMP_ALWAYS, 0x01, 0xff, 0x7f, D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE,
			                   D3DSTENCILOP_KEEP);
		//RCache.set_Stencil				(TRUE,D3DCMP_ALWAYS,0x00,0xff,0xff,D3DSTENCILOP_KEEP,D3DSTENCILOP_REPLACE,D3DSTENCILOP_KEEP);
		RCache.set_CullMode(CULL_CCW);
		RCache.set_ColorWriteEnable();
		GMBase.r_dsgraph_render_emissive(RImplementation.o.ssfx_bloom ? false : true);
	}

	if (RImplementation.o.ssfx_bloom)
	{
		// Render Emissive on `rt_ssfx_bloom_emissive`
		PIX_EVENT(SSFX_BLOOM_EMISSIVE);
		FLOAT ColorRGBA[4] = { 0,0,0,0 };
		HW.pContext->ClearRenderTargetView(Target->rt_ssfx_bloom_emissive->pRT, ColorRGBA);
		Target->u_setrt(Target->rt_ssfx_bloom_emissive, NULL, NULL, !RImplementation.o.dx10_msaa ? HW.pBaseZB : Target->rt_MSAADepth->pZRT);
		// Keep regular HUD geometry out of the emissive target. It is already
		// present in the scene texture sampled by the bloom build pass; drawing it
		// here again treats every strict-sorted HUD surface as an emissive source.
		GMBase.r_dsgraph_render_emissive(true, false);
	}

	// Lighting, non dependant on OCCQ
	{
		PIX_EVENT_C(DEFER_LIGHT_NO_OCCQ, dx10_marker_lights);
		Target->phase_accumulator();
		render_lights(LP_normal);
	}

	// Lighting, dependant on OCCQ
	{
		PIX_EVENT_C(DEFER_LIGHT_OCCQ, dx10_marker_lights);
		render_lights(LP_pending);
	}

	{
		PIX_EVENT(SSFX_VOLUMETRIC_BLUR);
		if (RImplementation.o.ssfx_volumetric)
			Target->phase_ssfx_volumetric_blur();
	}

	phase = PHASE_NORMAL;

	// Postprocess
	{
		PIX_EVENT_C(DEFER_LIGHT_COMBINE, dx10_marker_lights);
		Target->phase_combine();
	}

	if (Details)
		Details->details_clear();

	// Item and camera attachment UI: drawn in render_forward (upstream's place, under the sorted HUD) unless the
	// upscaler was on; then here, after the upscaler, at output resolution (neXt-Ray), so DLSS/FSR never smear it
	if (g_hud && !s_hud_ui_drawn)
	{
        PROF_EVENT("render_hud");
		PIX_EVENT(HUD_UI);
		if (g_hud->RenderActiveItemUIQuery())
		{
			Target->upscale_hud_ui_depth();
			GMBase.r_dsgraph_render_hud_ui();
		}
		if (g_hud->RenderCamAttachedUIQuery())
		{
			Target->upscale_hud_ui_depth();
			GMBase.r_dsgraph_render_cam_ui();
		}
	}

}
#include "../xrRender/CHudInitializer.h"

void CRender::RenderUI3DDirect()
{
	HW.pContext->ClearDepthStencilView(HW.pBaseZB, D3D_CLEAR_DEPTH | D3D_CLEAR_STENCIL, 1.0f, 0);

	RCache.set_Z(FALSE);
	RCache.set_RT(HW.pBaseRT, 0);
	GMUI3D.r_dsgraph_render_ui_3d();
}

bool CRender::GetUI3DIconAtlasInfo(u32& width, u32& height, float& resolution)
{
	if (!ps_r4_atlas || !Target || !Target->rt_ui_3d_icons)
		return false;
	width = Target->rt_ui_3d_icons->dwWidth;
	height = Target->rt_ui_3d_icons->dwHeight;
	resolution = clampr(ps_r4_atlas_resolution, 0.1f, 1.f);
	return true;
}

bool CRender::BeginUI3DIconAtlas()
{
	if (ui_3d_icon_atlas_active || !ps_r4_atlas || !Target || !Target->rt_ui_3d_icons)
		return false;

	ui_3d_icon_atlas_active = true;
	ui_3d_icon_project_saved = Device.mProject3DIcons;
	ui_3d_icon_full_transform_saved = Device.mFullTransform3DIcons;

	const u32 width = Target->rt_ui_3d_icons->dwWidth;
	const u32 height = Target->rt_ui_3d_icons->dwHeight;
	// Match the allocated atlas aspect ratio to keep model proportions intact.
	Device.mProject3DIcons.build_projection_ortho(UI_3D_ICON_ORTHO_HEIGHT * float(width) / float(height),
		UI_3D_ICON_ORTHO_HEIGHT, UI_3D_ICON_NEAR_PLANE, UI_3D_ICON_FAR_PLANE);
	Device.mFullTransform3DIcons.mul(Device.mProject3DIcons, Device.mView);

	const ref_rt& raw = Target->rt_ui_3d_icons_raw_msaa ?
		Target->rt_ui_3d_icons_raw_msaa : Target->rt_ui_3d_icons_raw;
	Target->u_setrt(raw, nullptr, nullptr, Target->rt_ui_3d_icons_depth->pZRT);
	Target->set_viewport_size(HW.pContext, width, height);

	const float clear_color[4] = {0.f, 0.f, 0.f, 0.f};
	HW.pContext->ClearRenderTargetView(raw->pRT, clear_color);
	HW.pContext->ClearDepthStencilView(Target->rt_ui_3d_icons_depth->pZRT,
		D3D_CLEAR_DEPTH | D3D_CLEAR_STENCIL, 1.f, 0);
	return true;
}

void CRender::BeginUI3DIconAtlasItem()
{
	// The MT graph retains every submission, including repeated visuals in different cells.
}

void CRender::EndUI3DIconAtlas()
{
	if (!ui_3d_icon_atlas_active)
		return;

	RCache.set_Z(FALSE);
	GMUI3D.r_dsgraph_render_ui_3d();

	// The postprocess expects an ordinary Texture2D. Resolve the multisampled
	// model atlas after all cells have been rendered, before sampling it.
	Target->u_setrt(Target->rt_ui_3d_icons->dwWidth, Target->rt_ui_3d_icons->dwHeight,
		nullptr, nullptr, nullptr, nullptr);
	if (Target->rt_ui_3d_icons_raw_msaa)
		HW.pContext->ResolveSubresource(Target->rt_ui_3d_icons_raw->pSurface, 0,
			Target->rt_ui_3d_icons_raw_msaa->pSurface, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
	Target->phase_ui_3d_icons_postprocess();

	Device.mProject3DIcons = ui_3d_icon_project_saved;
	Device.mFullTransform3DIcons = ui_3d_icon_full_transform_saved;
	Target->u_setrt(Device.dwWidth, Device.dwHeight, HW.pBaseRT, nullptr, nullptr, HW.pBaseZB);
	Target->set_viewport_size(HW.pContext, float(Device.dwWidth), float(Device.dwHeight));
	RCache.set_Z(FALSE);
	ui_3d_icon_atlas_active = false;
}

void CRender::render_forward()
{
	RImplementation.o.distortion = RImplementation.o.distortion_enabled; // enable distorion

	//******* Main render - second order geometry (the one, that doesn't support deffering)
	//.todo: should be done inside "combine" with estimation of of luminance, tone-mapping, etc.
	{
		// level
		phase = PHASE_NORMAL;
		//	Igor: we don't want to render old lods on next frame.
		GMBase.r_dsgraph_render_static(1); // normal level, secondary priority
		CParticlesAsync::Wait();
		GMBase.r_dsgraph_render_dynamic(1);
		GMBase.fade_render(); // faded-portals
		GMBase.r_dsgraph_render_sorted(false); // strict-sorted geoms
		g_pGamePersistent->Environment().RenderLast(); // rain/thunder-bolts

		// Item and camera attachment UI draws before the sorted HUD so nearer glass and the post chain cover it.
		// With the upscaler on, Render() draws it after the upscaler instead (neXt-Ray)
		if (g_hud && !upscale_scene_active)
		{
			PROF_EVENT("render_hud");
			if (g_hud->RenderActiveItemUIQuery())
				GMBase.r_dsgraph_render_hud_ui();
			if (g_hud->RenderCamAttachedUIQuery())
				GMBase.r_dsgraph_render_cam_ui();
			s_hud_ui_drawn = true;
		}

		GMBase.r_dsgraph_render_sorted_hud();
	}

	RImplementation.o.distortion = FALSE; // disable distorion
}

// Redotix99: for 3D Shader Based Scopes
void CRender::render_Reticle()
{
	VERIFY(0 == GMBase.RGraph.mapHUDSorted.Distort.size() + GMBase.RGraph.mapStaticSorted.Distort.size() + GMBase.RGraph.mapDynamicSorted.Distort.size());
	RImplementation.o.distortion = RImplementation.o.distortion_enabled;

	GMBase.r_dsgraph_render_ScopeSorted();

	RImplementation.o.distortion = FALSE;
}

void CRender::RenderToTarget(RRT target)
{
	ref_rt* RT = nullptr;

	switch (target)
	{
	case rtPDA:
		RT = &Target->rt_ui_pda;
		break;
	case rtSVP:
		RT = &Target->rt_secondVP;
		break;
	default:
		Debug.fatal(DEBUG_INFO, "None or wrong Target specified: %i", target);
		break;
	}

	ID3DTexture2D* pBuffer = nullptr;
#ifdef USE_DX12
	if (FAILED(HW.pDevice->GetBackBuffer(&pBuffer)) || !pBuffer)
		return;
#else
	HW.m_pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBuffer);
#endif
	HW.pContext->CopyResource((*RT)->pSurface, pBuffer);
	pBuffer->Release();
}
