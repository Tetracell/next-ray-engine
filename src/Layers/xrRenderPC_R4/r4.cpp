#include "stdafx.h"
#include "r4_vrs.h"
#include "../xrRender/shader_patches.h"
#include "r4.h"
#include "../xrRender/fbasicvisual.h"
#include "../../xrEngine/xr_object.h"
#include "../../xrEngine/CustomHUD.h"
#include "../../xrEngine/igame_persistent.h"
#include "../../xrEngine/environment.h"
#include "../xrRender/SkeletonCustom.h"
#include "../xrRender/LightTrack.h"
#include "../xrRender/dxRenderDeviceRender.h"
#include "../xrRender/dxWallMarkArray.h"
#include "../xrRender/dxUIShader.h"
#include "../xrRender/light_smap_keep.h"

#include "../xrRenderDX10/3DFluid/dx103DFluidManager.h"
#include "../xrRender/ShaderResourceTraits.h"

#include "../../xrCore/profiler.h"
#include "../../xrCore/ShaderSourceCRC.h"

#include "D3DX10Core.h"
#include "r4_upscaler.h"
#include "r4_perf_overlay.h"
#include "r4_frame_pacing.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

CRender RImplementation;

//////////////////////////////////////////////////////////////////////////
class CGlow : public IRender_Glow
{
public:
	bool bActive;
public:
	CGlow() : bActive(false)
	{
	}

	virtual void set_active(bool b) { bActive = b; }
	virtual bool get_active() { return bActive; }

	virtual void set_position(const Fvector& P, const float eps = EPS_L)
	{
	}

	virtual void set_direction(const Fvector& D)
	{
	}

	virtual void set_radius(float R)
	{
	}

	virtual void set_texture(LPCSTR name)
	{
	}

	virtual void set_color(const Fcolor& C)
	{
	}

	virtual void set_color(float r, float g, float b)
	{
	}
};

bool CRender::is_sun()
{
	if (o.sunstatic) return FALSE;
	Fcolor sun_color = ((light*)Lights.sun_adapted._get())->color;
	return (ps_r2_ls_flags.test(R2FLAG_SUN) && (u_diffuse2s(sun_color.r, sun_color.g, sun_color.b) > EPS));
}

float r_dtex_range = 50.f;
//////////////////////////////////////////////////////////////////////////
ShaderElement* CRender::rimp_select_sh_dynamic(dxRender_Visual* pVisual, float cdist_sq)
{
	int id = SE_R2_SHADOW;
	if (CRender::PHASE_NORMAL == RImplementation.phase)
	{
		id = ((_sqrt(cdist_sq) - pVisual->vis.sphere.R) < r_dtex_range) ? SE_R2_NORMAL_HQ : SE_R2_NORMAL_LQ;
	}
	return pVisual->shader->E[id]._get();
}

//////////////////////////////////////////////////////////////////////////
ShaderElement* CRender::rimp_select_sh_static(dxRender_Visual* pVisual, float cdist_sq)
{
	int id = SE_R2_SHADOW;
	if (CRender::PHASE_NORMAL == RImplementation.phase)
	{
		if (pVisual->shader->E[0]->flags.isLandscape)
		{
			float sec_dist = _sqrt(cdist_sq) - pVisual->vis.sphere.R;
			id = (sec_dist < ps_ssfx_terrain_quality.x * 10) ? SE_R2_NORMAL_HQ : SE_R2_NORMAL_LQ;
			
			// Very low shader variation
			if (sec_dist > 240)
				id = 3;
		}
		else
		{
			id = ((_sqrt(cdist_sq) - pVisual->vis.sphere.R) < r_dtex_range) ? SE_R2_NORMAL_HQ : SE_R2_NORMAL_LQ;
		}
	}
	return pVisual->shader->E[id]._get();
}

static class cl_parallax : public R_constant_setup
{
	virtual void setup(R_constant* C)
	{
		float h = ps_r2_df_parallax_h;
		RCache.set_c(C, h, -h / 2.f, 1.f / r_dtex_range, 1.f / r_dtex_range);
	}
} binder_parallax;

static class cl_LOD : public R_constant_setup
{
	virtual void setup(R_constant* C)
	{
		RCache.LOD.set_LOD(C);
	}
} binder_LOD;

static class cl_pos_decompress_params : public R_constant_setup
{
	virtual void setup(R_constant* C)
	{
		float VertTan = -1.0f * tanf(deg2rad(Device.fFOV / 2.0f));
		float HorzTan = - VertTan / Device.fASPECT;

		RCache.set_c(C, HorzTan, VertTan, (2.0f * HorzTan) / (float)Device.dwRenderWidth,
		             (2.0f * VertTan) / (float)Device.dwRenderHeight);
	}
} binder_pos_decompress_params;

static class cl_pos_decompress_params2 : public R_constant_setup
{
	virtual void setup(R_constant* C)
	{
		RCache.set_c(C, (float)Device.dwRenderWidth, (float)Device.dwRenderHeight, 1.0f / (float)Device.dwRenderWidth,
		             1.0f / (float)Device.dwRenderHeight);
	}
} binder_pos_decompress_params2;

static class cl_water_intensity : public R_constant_setup
{
	virtual void setup(R_constant* C)
	{
		CEnvDescriptor& E = *g_pGamePersistent->Environment().CurrentEnv;
		float fValue = E.m_fWaterIntensity;
		RCache.set_c(C, fValue, fValue, fValue, 0);
	}
} binder_water_intensity;

#ifdef TREE_WIND_EFFECT
static class cl_tree_amplitude_intensity : public R_constant_setup
{
	virtual void setup(R_constant* C)
	{
		CEnvDescriptor& E = *g_pGamePersistent->Environment().CurrentEnv;
		float fValue = E.m_fTreeAmplitudeIntensity;
		RCache.set_c(C, fValue, fValue, fValue, 0);
	}
} binder_tree_amplitude_intensity;
#endif

static class cl_sun_shafts_intensity : public R_constant_setup
{
	virtual void setup(R_constant* C)
	{
		CEnvDescriptor& E = *g_pGamePersistent->Environment().CurrentEnv;
		float fValue = E.m_fSunShaftsIntensity;
		RCache.set_c(C, fValue, fValue, fValue, 0);
	}
} binder_sun_shafts_intensity;

static class cl_alpha_ref : public R_constant_setup
{
	virtual void setup(R_constant* C)
	{
		StateManager.BindAlphaRef(C);
	}
} binder_alpha_ref;

static class cl_meatchunks_stuff : public R_constant_setup
{
    virtual void setup(R_constant* C)
    {
        CEnvDescriptor& E = *g_pGamePersistent->Environment().CurrentEnv;
        float fValue1 = E.m_fHemiVibrance;
		float fValue2 = E.m_fHemiContrast;
		float fValue3 = E.m_fWetSurfaces;

        RCache.set_c(C, fValue1, fValue2, fValue3, 0);
    }
} binder_meatchunks_stuff;


extern ENGINE_API BOOL r2_sun_static;
extern ENGINE_API BOOL r2_advanced_pp; //	advanced post process and effects
//////////////////////////////////////////////////////////////////////////
// Just two static storage
void CRender::create()
{
	Device.seqFrame.Add(this,REG_PRIORITY_HIGH + 0x12345678);

	Engine.External.SetSkinningMode();
	m_MSAASample = -1;
    o.volsize = 8;
	if (Core.ParamsData.test(ECoreParams::volumetric1)) o.volsize = 1;
    if (Core.ParamsData.test(ECoreParams::volumetric2)) o.volsize = 2;
    if (Core.ParamsData.test(ECoreParams::volumetric4)) o.volsize = 4;

	// hardware
	o.smapsize = 2048;
	o.mrt = (HW.Caps.raster.dwMRT_count >= 3);
	o.mrtmixdepth = (HW.Caps.raster.b_MRT_mixdepth);

	// Check for NULL render target support
	//	DX10 disabled
	//D3DFORMAT	nullrt	= (D3DFORMAT)MAKEFOURCC('N','U','L','L');
	//o.nullrt			= HW.support	(nullrt,			D3DRTYPE_SURFACE, D3DUSAGE_RENDERTARGET);
	o.nullrt = false;
	/*
	if (o.nullrt)		{
	Msg				("* NULLRT supported and used");
	};
	*/
	if (o.nullrt)
	{
		Msg("* NULLRT supported");

		//.	    _tzset			();
		//.		??? _strdate	( date, 128 );	???
		//.		??? if (date < 22-march-07)		
		if (0)
		{
			u32 device_id = HW.Caps.id_device;
			bool disable_nullrt = false;
			switch (device_id)
			{
			case 0x190:
			case 0x191:
			case 0x192:
			case 0x193:
			case 0x194:
			case 0x197:
			case 0x19D:
			case 0x19E:
				{
					disable_nullrt = true; //G80
					break;
				}
			case 0x400:
			case 0x401:
			case 0x402:
			case 0x403:
			case 0x404:
			case 0x405:
			case 0x40E:
			case 0x40F:
				{
					disable_nullrt = true; //G84
					break;
				}
			case 0x420:
			case 0x421:
			case 0x422:
			case 0x423:
			case 0x424:
			case 0x42D:
			case 0x42E:
			case 0x42F:
				{
					disable_nullrt = true; // G86
					break;
				}
			}
			if (disable_nullrt) o.nullrt = false;
		};
		if (o.nullrt) Msg("* ...and used");
	};


	// SMAP / DST
	o.HW_smap_FETCH4 = FALSE;
	//	DX10 disabled
	//o.HW_smap			= HW.support	(D3DFMT_D24X8,			D3DRTYPE_TEXTURE,D3DUSAGE_DEPTHSTENCIL);
	o.HW_smap = true;
	o.HW_smap_PCF = o.HW_smap;
	if (o.HW_smap)
	{
		//	For ATI it's much faster on DX10 to use D32F format
		if (HW.Caps.id_vendor == 0x1002)
			o.HW_smap_FORMAT = D3DFMT_D32F_LOCKABLE;
		else
			o.HW_smap_FORMAT = D3DFMT_D24X8;
		Msg("* HWDST/PCF supported and used");
	}

	//	DX10 disabled
	//o.fp16_filter		= HW.support	(D3DFMT_A16B16G16R16F,	D3DRTYPE_TEXTURE,D3DUSAGE_QUERY_FILTER);
	//o.fp16_blend		= HW.support	(D3DFMT_A16B16G16R16F,	D3DRTYPE_TEXTURE,D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING);
	o.fp16_filter = true;
	o.fp16_blend = true;

	// search for ATI formats
	if (!o.HW_smap && !Core.ParamsData.test(ECoreParams::nodf24))
	{
		o.HW_smap = HW.support((D3DFORMAT)(MAKEFOURCC('D', 'F', '2', '4')), D3DRTYPE_TEXTURE, D3DUSAGE_DEPTHSTENCIL);
		if (o.HW_smap)
		{
			o.HW_smap_FORMAT = MAKEFOURCC('D', 'F', '2', '4');
			o.HW_smap_PCF = FALSE;
			o.HW_smap_FETCH4 = TRUE;
		}
		Msg("* DF24/F4 supported and used [%X]", o.HW_smap_FORMAT);
	}

	// emulate ATI-R4xx series
	if (Core.ParamsData.test(ECoreParams::r4xx))
	{
		o.mrtmixdepth = FALSE;
		o.HW_smap = FALSE;
		o.HW_smap_PCF = FALSE;
		o.fp16_filter = FALSE;
		o.fp16_blend = FALSE;
	}

	VERIFY2(o.mrt && (HW.Caps.raster.dwInstructions>=256), "Hardware doesn't meet minimum feature-level");
	if (o.mrtmixdepth) o.albedo_wo = FALSE;
	else if (o.fp16_blend) o.albedo_wo = FALSE;
	else o.albedo_wo = TRUE;

	// nvstencil on NV40 and up
	o.nvstencil = FALSE;
	if (Core.ParamsData.test(ECoreParams::nonvs))
		o.nvstencil = FALSE;

	// nv-dbt
	//	DX10 disabled
	//o.nvdbt				= HW.support	((D3DFORMAT)MAKEFOURCC('N','V','D','B'), D3DRTYPE_SURFACE, 0);
	o.nvdbt = false;
	if (o.nvdbt) Msg("* NV-DBT supported and used");

	o.no_ram_textures = Core.ParamsData.test(ECoreParams::noramtex) ? TRUE : ps_r__common_flags.test(RFLAG_NO_RAM_TEXTURES);
	if (o.no_ram_textures)
		Msg("* Managed textures disabled");
	else
		Msg("* Managed textures enabled");

	// options (smap-pool-size)
	if (Core.ParamsData.test(ECoreParams::smap1536)) o.smapsize = 1536;
	if (Core.ParamsData.test(ECoreParams::smap2048)) o.smapsize = 2048;
	if (Core.ParamsData.test(ECoreParams::smap2560)) o.smapsize = 2560;
	if (Core.ParamsData.test(ECoreParams::smap3072)) o.smapsize = 3072;
	if (Core.ParamsData.test(ECoreParams::smap4096)) o.smapsize = 4096;

	// gloss
	char* g = strstr(Core.Params, "-gloss ");
	o.forcegloss = Core.ParamsData.test(ECoreParams::gloss);
	if (g)
	{
		o.forcegloss_v = float(atoi(g + xr_strlen("-gloss "))) / 255.f;
	}

	// options
	o.bug = Core.ParamsData.test(ECoreParams::bug);
	o.sunfilter = Core.ParamsData.test(ECoreParams::sunfilter);
	o.sunstatic = r2_sun_static;
	o.advancedpp = r2_advanced_pp;
	o.volumetricfog = ps_r2_ls_flags.test(R3FLAG_VOLUMETRIC_SMOKE);
	o.sjitter = Core.ParamsData.test(ECoreParams::sjitter);
	o.depth16 = Core.ParamsData.test(ECoreParams::depth16);
	o.noshadows = Core.ParamsData.test(ECoreParams::noshadows) || Core.ParamsData.test(ECoreParams::r4_dev);
	o.Tshadows = Core.ParamsData.test(ECoreParams::tsh);
	o.distortion_enabled = !Core.ParamsData.test(ECoreParams::nodistort);
	o.distortion = o.distortion_enabled;
	o.disasm = Core.ParamsData.test(ECoreParams::disasm);
	o.forceskinw = Core.ParamsData.test(ECoreParams::skinw);

	o.ssao_blur_on = ps_r2_ls_flags_ext.test(R2FLAGEXT_SSAO_BLUR) && (ps_r_ssao != 0);
	o.ssao_opt_data = ps_r2_ls_flags_ext.test(R2FLAGEXT_SSAO_OPT_DATA) && (ps_r_ssao != 0);
	o.ssao_half_data = ps_r2_ls_flags_ext.test(R2FLAGEXT_SSAO_HALF_DATA) && o.ssao_opt_data && (ps_r_ssao != 0);
	o.ssao_hdao = ps_r2_ls_flags_ext.test(R2FLAGEXT_SSAO_HDAO) && (ps_r_ssao != 0);
	o.ssao_hbao = !o.ssao_hdao && ps_r2_ls_flags_ext.test(R2FLAGEXT_SSAO_HBAO) && (ps_r_ssao != 0);

	//	TODO: fix hbao shader to allow to perform per-subsample effect!
	o.hbao_vectorized = false;
	if (o.ssao_hbao)
	{
		if (HW.Caps.id_vendor == 0x1002)
			o.hbao_vectorized = true;
		o.ssao_opt_data = true;
	}

	if (o.ssao_hdao)
		o.ssao_opt_data = false;

	o.dx10_sm4_1 = ps_r2_ls_flags.test((u32)R3FLAG_USE_DX10_1);
	o.dx10_sm4_1 = o.dx10_sm4_1 && (HW.FeatureLevel >= D3D_FEATURE_LEVEL_10_1);

	// HDR10
	o.dx11_hdr10 = !!ps_r4_hdr10_on;

	// Upscaler (DLSS / FSR): needs the SSS motion vectors and a single-sampled scene
	o.upscaler = upscaler_check();
	upscaler_probe();

	//	MSAA option dependencies
	o.dx10_msaa = ps_r3_msaa && !o.dx11_hdr10 && !o.upscaler;
	o.dx10_msaa_samples = (o.dx11_hdr10 || o.upscaler) ? 1 : (1 << ps_r3_msaa);

	// HDR output's scene highlights: the DX12 device converts at Present (the swap chain is up by now)
#ifdef USE_DX12
	o.nextray_hdr_scene = ps_r4_hdr_scene && HW.pDevice->HdrOutput() && !o.dx11_hdr10 && !o.dx10_msaa;
#else
	o.nextray_hdr_scene = false;
#endif

	o.dx10_msaa_opt = ps_r2_ls_flags.test(R3FLAG_MSAA_OPT);
	o.dx10_msaa_opt = o.dx10_msaa_opt && o.dx10_msaa && (HW.FeatureLevel >= D3D_FEATURE_LEVEL_10_1)
		|| o.dx10_msaa && (HW.FeatureLevel >= D3D_FEATURE_LEVEL_11_0);

	//o.dx10_msaa_hybrid	= ps_r2_ls_flags.test(R3FLAG_MSAA_HYBRID);
	o.dx10_msaa_hybrid = ps_r2_ls_flags.test((u32)R3FLAG_USE_DX10_1);
	o.dx10_msaa_hybrid &= !o.dx10_msaa_opt && o.dx10_msaa && (HW.FeatureLevel >= D3D_FEATURE_LEVEL_10_1);

	//	Allow alpha test MSAA for DX10.0

	//o.dx10_msaa_alphatest= ps_r2_ls_flags.test((u32)R3FLAG_MSAA_ALPHATEST);
	//o.dx10_msaa_alphatest= o.dx10_msaa_alphatest && o.dx10_msaa;

	//o.dx10_msaa_alphatest_atoc= (o.dx10_msaa_alphatest && !o.dx10_msaa_opt && !o.dx10_msaa_hybrid);

	o.dx10_msaa_alphatest = 0;
	if (o.dx10_msaa)
	{
		if (o.dx10_msaa_opt || o.dx10_msaa_hybrid)
		{
			if (ps_r3_msaa_atest == 1)
				o.dx10_msaa_alphatest = MSAA_ATEST_DX10_1_ATOC;
			else if (ps_r3_msaa_atest == 2)
				o.dx10_msaa_alphatest = MSAA_ATEST_DX10_1_NATIVE;
		}
		else
		{
			if (ps_r3_msaa_atest)
				o.dx10_msaa_alphatest = MSAA_ATEST_DX10_0_ATOC;
		}
	}


	o.dx10_minmax_sm = ps_r3_minmax_sm;
	o.dx10_minmax_sm_screenarea_threshold = 1600 * 1200;

	o.dx11_enable_tessellation = HW.FeatureLevel >= D3D_FEATURE_LEVEL_11_0 && ps_r2_ls_flags_ext.test(
		R2FLAGEXT_ENABLE_TESSELLATION);

	if (o.dx10_minmax_sm == MMSM_AUTODETECT)
	{
		o.dx10_minmax_sm = MMSM_OFF;

		//	AMD device
		if (HW.Caps.id_vendor == 0x1002)
		{
			if (ps_r_sun_quality >= 3)
				o.dx10_minmax_sm = MMSM_AUTO;
			else if (ps_r_sun_shafts >= 2)
			{
				o.dx10_minmax_sm = MMSM_AUTODETECT;
				//	Check resolution in runtime in use_minmax_sm_this_frame
				o.dx10_minmax_sm_screenarea_threshold = 1600 * 1200;
			}
		}

		//	NVidia boards
		if (HW.Caps.id_vendor == 0x10DE)
		{
			if ((ps_r_sun_shafts >= 2))
			{
				o.dx10_minmax_sm = MMSM_AUTODETECT;
				//	Check resolution in runtime in use_minmax_sm_this_frame
				o.dx10_minmax_sm_screenarea_threshold = 1280 * 1024;
			}
		}
	}

	// Check if SSS shaders exist
	string_path fn;
	o.ssfx_core = FS.exist(fn, "$game_shaders$", "r3\\screenspace_common", ".h") ? 1 : 0;
	o.ssfx_rain = FS.exist(fn, "$game_shaders$", "r3\\effects_rain_splash", ".ps") ? 1 : 0;
	o.ssfx_blood = FS.exist(fn, "$game_shaders$", "r3\\effects_wallmark_blood", ".ps") ? 1 : 0;
	o.ssfx_branches = FS.exist(fn, "$game_shaders$", "r3\\deffer_tree_branch_aref_bump-hq", ".ps") ? 1 : 0;
	o.ssfx_hud_raindrops = FS.exist(fn, "$game_shaders$", "r3\\deffer_base_hud_bump", ".ps") ? 1 : 0;
	o.ssfx_ssr = FS.exist(fn, "$game_shaders$", "r3\\ssfx_ssr", ".ps") ? 1 : 0;
	o.ssfx_terrain = FS.exist(fn, "$game_shaders$", "r3\\deffer_terrain_high_flat_d", ".ps") ? 1 : 0;
	o.ssfx_volumetric = FS.exist(fn, "$game_shaders$", "r3\\ssfx_volumetric_blur", ".ps") ? 1 : 0;
	o.ssfx_water = FS.exist(fn, "$game_shaders$", "r3\\ssfx_water", ".ps") ? 1 : 0;
	o.ssfx_ao = FS.exist(fn, "$game_shaders$", "r3\\ssfx_ao", ".ps") ? 1 : 0;
	o.ssfx_il = FS.exist(fn, "$game_shaders$", "r3\\ssfx_il", ".ps") ? 1 : 0;
	o.ssfx_sss = FS.exist(fn, "$game_shaders$", "r3\\ssfx_sss", ".ps") ? 1 : 0;
	o.ssfx_bloom = FS.exist(fn, "$game_shaders$", "r3\\ssfx_bloom", ".ps") ? 1 : 0;
	o.ssfx_taa = FS.exist(fn, "$game_shaders$", "r3\\ssfx_taa", ".ps") ? 1 : 0;
	o.ssfx_fog = FS.exist(fn, "$game_shaders$", "r3\\ssfx_fog_scattering", ".ps") ? 1 : 0;
	o.ssfx_motionblur = FS.exist(fn, "$game_shaders$", "r3\\ssfx_motion_blur", ".ps") ? 1 : 0;
	o.ssfx_motionvectors = FS.exist(fn, "$game_shaders$", "r3\\screenspace_mvectors", ".h") ? 1 : 0;
	o.ssfx_glass = FS.exist(fn, "$game_shaders$", "r3\\ssfx_glass", ".ps") ? 1 : 0; 

	Msg("- Supports SSS UPDATE 23");
	Msg("- SSS CORE INSTALLED %i", o.ssfx_core);
	Msg("- SSS HUD SHADER INSTALLED %i", o.ssfx_hud_raindrops);
	Msg("- SSS MOTION VECTORS SHADER INSTALLED %i", o.ssfx_motionvectors);
	Msg("- SSS RAIN SHADER INSTALLED %i", o.ssfx_rain);
	Msg("- SSS BLOOD SHADER INSTALLED %i", o.ssfx_blood);
	Msg("- SSS BRANCHES SHADER INSTALLED %i", o.ssfx_branches);
	Msg("- SSS SSR SHADER INSTALLED %i", o.ssfx_ssr);
	Msg("- SSS TERRAIN SHADER INSTALLED %i", o.ssfx_terrain);
	Msg("- SSS VOLUMETRIC SHADER INSTALLED %i", o.ssfx_volumetric);
	Msg("- SSS WATER SHADER INSTALLED %i", o.ssfx_water);
	Msg("- SSS AO SHADER INSTALLED %i", o.ssfx_ao);
	Msg("- SSS IL SHADER INSTALLED %i", o.ssfx_il);
	Msg("- SSS SSS SHADER INSTALLED %i", o.ssfx_sss);
	Msg("- SSS BLOOM SHADER INSTALLED %i", o.ssfx_bloom);
	Msg("- SSS FOG SHADER INSTALLED %i", o.ssfx_fog);
	Msg("- SSS GLASS SHADER INSTALLED %i", o.ssfx_glass);
	Msg("- SSS MOTION BLUR SHADER INSTALLED %i", o.ssfx_motionblur);
	Msg("- SSS TAA SHADER INSTALLED %i", o.ssfx_taa);

	// constants
	CResourceManager* RM = dxRenderDeviceRender::Instance().Resources;
	RM->RegisterConstantSetup("parallax", &binder_parallax);
	RM->RegisterConstantSetup("water_intensity", &binder_water_intensity);
	RM->RegisterConstantSetup("sun_shafts_intensity", &binder_sun_shafts_intensity);
	RM->RegisterConstantSetup("m_AlphaRef", &binder_alpha_ref);
	RM->RegisterConstantSetup("pos_decompression_params", &binder_pos_decompress_params);
	RM->RegisterConstantSetup("pos_decompression_params2", &binder_pos_decompress_params2);
	RM->RegisterConstantSetup("triLOD", &binder_LOD);
	RM->RegisterConstantSetup("hmodel_stuff", &binder_meatchunks_stuff);

	c_lmaterial = "L_material";
	c_sbase = "s_base";

	m_bMakeAsyncSS = false;

	Target = xr_new<CRenderTarget>(); // Main target

	Models = xr_new<CModelPool>();
	PSLibrary.OnCreate();
	HWOCC.occq_create(occq_size);

	rmNormal();

	GMBase.initialize();
	FluidManager.Initialize(70, 70, 70);
	//	FluidManager.Initialize( 100, 100, 100 );
	FluidManager.SetScreenSize(Target->upscale_render_width(), Target->upscale_render_height()); // scene resolution

	Device.ModelDefferClear = xr_make_delegate(Models, &CModelPool::DeleteQueuedDeffer);

	// Just before ImGui itself renders (priority -5)
	Device.seqRender.Add(&g_PerfOverlay, -4);
	Device.seqRender.Add(&g_ShaderProgressOverlay, -4);
}

namespace hiz { void destroy(); }
namespace sun_cache { void destroy(); }

namespace tex_streaming { void update(); void clear(); } // dx10SH_Texture.cpp

namespace shader_warmup { void stop(); }
namespace shader_precompile { void stop(); }
extern void tree_instancing_reset();

void CRender::destroy()
{
	tree_instancing_reset();
	shader_warmup::stop(); // before the device goes
	shader_precompile::stop();
	tex_streaming::clear();
	hiz::destroy();
#ifdef USE_DX12
	vrs::release();
#endif
	light_smap_keep::destroy();
	sun_cache::destroy();
	m_bMakeAsyncSS = false;
	FluidManager.Destroy();
	GMBase.destroy();

	HWOCC.occq_destroy();
	xr_delete(Models);
	xr_delete(Target);
	for (u32 type = upscaler_dlss; type < upscaler_count; ++type)
		if (CUpscaler* upscaler = GetUpscaler(type))
			ReleaseUpscaler(upscaler);
	Device.seqRender.Remove(&g_PerfOverlay);
	Device.seqRender.Remove(&g_ShaderProgressOverlay);
	g_PerfOverlay.Destroy();
	frame_pacing::destroy();
	PSLibrary.OnDestroy();
	Device.seqFrame.Remove(this);
	Device.ModelDefferClear = nullptr;
}

void CRender::reset_begin()
{
	tree_instancing_reset();
	//AVO: let's reload details while changed details options on vid_restart
	if (b_loaded && ((dm_current_size != dm_size) || (ps_r__Detail_density != ps_current_detail_density) || (
		ps_r__Detail_height != ps_current_detail_height)))
	{
		Details->Unload();
		xr_delete(Details);
	}
	//-AVO

	xr_delete(Target);
	HWOCC.occq_destroy();
}

// Upscalers need the Screen Space Shaders motion vectors; DLSS also needs an NVIDIA RTX GPU
// Which upscalers this GPU supports, for the options menu (r4_upscaler_available). DLSS is only
// tried on NVIDIA adapters (DXVK reports NVIDIA GPUs as AMD unless NVAPI is enabled) and released
// again when not in use (ReleaseUpscaler: kept loaded with OptiScaler), so the menu offers it only where it can start
void CRender::upscaler_probe()
{
	ps_r4_upscaler_available = 0;
	if (CUpscaler* fsr = GetUpscaler(upscaler_fsr))
		if (fsr->Init())
			ps_r4_upscaler_available |= 2;

	if (o.upscaler == upscaler_dlss)
		ps_r4_upscaler_available |= 1;
	else if (HW.Caps.id_vendor == 0x10DE)
	{
		if (CUpscaler* dlss = GetUpscaler(upscaler_dlss))
		{
			if (dlss->Init())
				ps_r4_upscaler_available |= 1;
			ReleaseUpscaler(dlss);
		}
	}
	// XeSS (DX12 renderer): any GPU with Shader Model 6.4; released again when not in use, like DLSS
	if (o.upscaler == upscaler_xess)
		ps_r4_upscaler_available |= 4;
	else if (CUpscaler* xess = GetUpscaler(upscaler_xess))
	{
		if (xess->Init())
			ps_r4_upscaler_available |= 4;
		ReleaseUpscaler(xess);
	}
	Msg("* Upscalers available: %s%s%s%s", (ps_r4_upscaler_available & 1) ? "DLSS " : "",
	    (ps_r4_upscaler_available & 2) ? "FSR " : "", (ps_r4_upscaler_available & 4) ? "XeSS" : "",
	    ps_r4_upscaler_available ? "" : "none");
}

u32 CRender::upscaler_check()
{
	CUpscaler* upscaler = GetUpscaler(ps_r4_upscaler);
	if (!upscaler)
		return upscaler_off;

	// The upscalers are set up for tonemapped SDR input; with HDR10 the scene color is not tonemapped yet
	if (o.dx11_hdr10)
	{
		Msg("! %s: disabled, not supported together with HDR10 (r4_hdr10_on)", upscaler->Name());
		return upscaler_off;
	}

	string_path mv_fn;
	if (!FS.exist(mv_fn, "$game_shaders$", "r3\\screenspace_mvectors", ".h"))
	{
		Msg("! %s: disabled, Screen Space Shaders motion vectors (r3\\screenspace_mvectors.h) are not installed", upscaler->Name());
		return upscaler_off;
	}
	return upscaler->Init() ? ps_r4_upscaler : upscaler_off;
}

void CRender::reset_end()
{
	HWOCC.occq_create(occq_size);

	// The upscaler can be switched with vid_restart, unless MSAA was enabled when the renderer started
	if (ps_r4_upscaler != o.upscaler)
	{
		const u32 previous = o.upscaler;
		if (o.dx10_msaa)
			Msg("! Upscaler: turn off MSAA (r3_msaa) and restart the game to use DLSS or FSR");
		else
			o.upscaler = upscaler_check();

		// Free the one no longer used (e.g. NGX when switching from DLSS to FSR)
		if (previous != o.upscaler)
			if (CUpscaler* old_upscaler = GetUpscaler(previous))
				ReleaseUpscaler(old_upscaler);

		CUpscaler* upscaler = GetUpscaler(o.upscaler);
		Msg("* Upscaler: %s", upscaler ? upscaler->Name() : "off");
	}

	Target = xr_new<CRenderTarget>();

	//AVO: let's reload details while changed details options on vid_restart
	if (b_loaded && ((dm_current_size != dm_size) || (ps_r__Detail_density != ps_current_detail_density) || (
		ps_r__Detail_height != ps_current_detail_height)))
	{
		Details = xr_new<CDetailManager>();
		Details->Load();
	}
	//-AVO

	FluidManager.SetScreenSize(Target->upscale_render_width(), Target->upscale_render_height()); // scene resolution

	// Set this flag true to skip the first render frame,
	// that some data is not ready in the first frame (for example device camera position)
	m_bFirstFrameAfterReset = true;
}

// Shader warm-up (r4_shader_warmup): at the first frame of the game (the main menu), a background thread creates every
// shader of the engine's shader cache that matches the current settings. Under DXVK a shader's translation and its
// pipeline libraries are compiled when it is created and kept for the device's lifetime, so the level's own creation
// later is a lookup instead of a compile in the frame that first draws with it; native drivers fill their own caches.
// The cache files carry one variant per settings combination, named by the settings (the same for every shader of
// a session): the name of the last compiled shader picks them. Level loads are not touched.
int ps_r4_shader_warmup = 1;
// r4_shader_patches: bits of the neXt-Ray patch mod's shader changes in use, the shaders get it as NEXTRAY_PATCHES
// (each change is kept next to the original code and picked by its bit; vid_restart recompiles). In the variant name
int ps_r4_shader_patches = 0xDFF; // 0x010 (IL) unused: its SampleLevel change measured slower and was dropped; 0x200 unused (the 3DSS lens jitter correction, replaced by r4_upscaler_scope_late); 0x400 SSR sky taken in the SSR pass where it shows; 0x800 SSR gloss sky taps only where used
namespace shader_warmup
{
	static std::thread s_thread;
	static std::atomic<bool> s_stop{ false };
	static bool s_started = false;
	static string_path s_variant = "";

	void note_variant(LPCSTR name) { xr_strcpy(s_variant, name); }

	static void run(xr_string root, xr_string variant)
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		CTimer timer;
		timer.Start();
		u32 created = 0, missing = 0, failed = 0;
		WIN32_FIND_DATAA fd;
		HANDLE find = FindFirstFileA((root + "*").c_str(), &fd);
		if (find == INVALID_HANDLE_VALUE)
			return;

		// The shaders first (a folder each, named by the source file), so the progress shows their number
		struct Item
		{
			xr_string path;
			char type;
		};
		xr_vector<Item> items;
		do
		{
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.')
				continue;
			const char* dot = strrchr(fd.cFileName, '.');
			if (!dot || xr_strlen(dot) != 3)
				continue;
			items.push_back({ root + fd.cFileName + "\\" + variant, dot[1] });
		} while (FindNextFileA(find, &fd));
		FindClose(find);

		shader_progress::begin("Compiling shaders", u32(items.size()));
		xr_vector<u8> buf;
		for (const Item& item : items)
		{
			if (s_stop)
				break;
			struct Step { ~Step() { shader_progress::step(); } } counted; // also on continue
			const xr_string& path = item.path;
			HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
			if (file == INVALID_HANDLE_VALUE)
			{
				missing++;
				continue;
			}
			const DWORD size = GetFileSize(file, nullptr);
			DWORD read = 0;
			buf.resize(size > 4 ? size : 4);
			const BOOL ok = size > 8 && size < 16 * 1024 * 1024 && ReadFile(file, buf.data(), size, &read, nullptr);
			CloseHandle(file);
			if (!ok || read != size || *(const u32*)buf.data() != crc32(buf.data() + 4, size - 4))
			{
				failed++;
				continue;
			}
			const void* code = buf.data() + 4;
			const SIZE_T len = size - 4;
			HRESULT hr = E_FAIL;
			switch (item.type)
			{
			case 'p': { ID3DPixelShader* sh = nullptr; hr = HW.pDevice->CreatePixelShader(code, len, nullptr, &sh); _RELEASE(sh); } break;
			case 'v': { ID3DVertexShader* sh = nullptr; hr = HW.pDevice->CreateVertexShader(code, len, nullptr, &sh); _RELEASE(sh); } break;
			case 'g': { ID3DGeometryShader* sh = nullptr; hr = HW.pDevice->CreateGeometryShader(code, len, nullptr, &sh); _RELEASE(sh); } break;
			case 'h': { ID3DHullShader* sh = nullptr; hr = HW.pDevice->CreateHullShader(code, len, nullptr, &sh); _RELEASE(sh); } break;
			case 'd': { ID3DDomainShader* sh = nullptr; hr = HW.pDevice->CreateDomainShader(code, len, nullptr, &sh); _RELEASE(sh); } break;
			case 'c': { ID3DComputeShader* sh = nullptr; hr = HW.pDevice->CreateComputeShader(code, len, nullptr, &sh); _RELEASE(sh); } break;
			default: continue;
			}
			SUCCEEDED(hr) ? created++ : failed++;
		}
		shader_progress::end(!s_stop);
		Msg("* Shader warm-up: %u shaders created in %.2f s (%u without a cached variant for these settings, %u failed), variant %s",
			created, timer.GetElapsed_sec(), missing, failed, variant.c_str());
	}

	// From the first frame: once per process, when a shader was compiled this session
	void start_once()
	{
#ifdef USE_DX12
		return; // shader objects are only CPU data under D3D12: what is worth warming there are pipeline states
#endif
		if (s_started || !ps_r4_shader_warmup || !s_variant[0])
			return;
		s_started = true;
		string_path root;
		FS.update_path(root, "$app_data_root$", "shaders_cache\\r4\\");
		s_stop = false;
		s_thread = std::thread(run, xr_string(root), xr_string(s_variant));
	}

	void stop()
	{
		s_stop = true;
		if (s_thread.joinable())
			s_thread.join();
	}
}

// Shader pre-compilation (r4_shader_precompile): the compiled shader cache holds one variant per settings and shader
// sources; after a settings change or a shader mod update every shader the game draws is compiled again on the main
// thread, at the level's load and in the frame that first draws it (hitches of 0.1-1 s each, DX11 and DX12 alike).
// From the first frame with new settings (the main menu, or after a vid_restart), background threads compile ahead
// every shader earlier runs compiled (the cache's folders, each skinning mode they had) for the current settings, so
// those find compiled code. The main thread, needing one a thread is compiling, waits for it instead of compiling it
// again. Shaders patched in memory or with per-material options (tessellation) are left to the main thread
int ps_r4_shader_precompile = 1;
namespace shader_precompile
{
	thread_local int t_skinning = INT_MIN; // a skinning mode to compile for, INT_MIN the engine's
	thread_local bool t_ahead = false; // shader_compile writes the cache file only, no shader object
	thread_local bool t_variant_only = false; // shader_compile only names the variant (t_variant)
	thread_local string_path t_variant = "";

	static std::mutex s_lock;
	static std::condition_variable s_done;
	static xr_set<xr_string> s_busy; // cache files a thread is compiling
	static std::vector<std::thread> s_threads;
	static std::atomic<bool> s_stop{ false };
	static std::atomic<u32> s_next{ 0 }, s_compiled{ 0 }, s_failed{ 0 };
	static string_path s_started_variant = "";
	static CTimer s_timer;

	struct Job
	{
		xr_string name; // as shader_compile gets it ("deffer_model_bump_2" for skinning mode 2)
		xr_string source; // the file under the shaders' folder
		char type; // v p g h d c
		int skinning;
	};
	static xr_vector<Job> s_jobs;

	// The thread compiling file waits until it's written (a main-thread compile waits rather than compiling twice)
	bool begin(LPCSTR file)
	{
		std::lock_guard<std::mutex> lock(s_lock);
		return s_busy.insert(file).second;
	}

	void finish(LPCSTR file)
	{
		{
			std::lock_guard<std::mutex> lock(s_lock);
			s_busy.erase(file);
		}
		s_done.notify_all();
	}

	// Main thread: true when a thread was compiling it (then it's in the cache, unless it failed)
	bool wait_for(LPCSTR file)
	{
		std::unique_lock<std::mutex> lock(s_lock);
		if (!s_busy.count(file))
			return false;
		const u64 start = GetTickCount64();
		s_done.wait_for(lock, std::chrono::seconds(30), [file] { return !s_busy.count(file); });
		Msg("* Shader pre-compilation: waited %u ms for %s", u32(GetTickCount64() - start), file);
		return true;
	}

	static void worker()
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		t_ahead = true;
		for (u32 i; !s_stop && (i = s_next++) < s_jobs.size();)
		{
			struct Step { ~Step() { shader_progress::step(); } } counted;
			const Job& job = s_jobs[i];
			string_path cname;
			strconcat(sizeof(cname), cname, ::Render->getShaderPath(), job.source.c_str());
			IReader* file = shader_patches::open(cname);
			if (!file)
				continue;
			const u32 size = file->length();
			xr_vector<char> data(size + 1);
			CopyMemory(data.data(), file->pointer(), size);
			data[size] = 0;
			FS.r_close(file);
			// The target and entry the renderer's _Create* would use
			LPCSTR target = "vs_2_0", entry = "main";
			switch (job.type)
			{
			case 'v':
				if (strstr(data.data(), "main_vs_1_1")) target = "vs_1_1", entry = "main_vs_1_1";
				if (strstr(data.data(), "main_vs_2_0")) target = "vs_2_0", entry = "main_vs_2_0";
				break;
			case 'p':
				target = "ps_2_0";
				for (LPCSTR v : { "ps_1_1", "ps_1_2", "ps_1_3", "ps_1_4", "ps_2_0" })
				{
					static const struct { LPCSTR target, entry; } entries[] = { { "ps_1_1", "main_ps_1_1" },
						{ "ps_1_2", "main_ps_1_2" }, { "ps_1_3", "main_ps_1_3" }, { "ps_1_4", "main_ps_1_4" },
						{ "ps_2_0", "main_ps_2_0" } };
					for (const auto& e : entries)
						if (!xr_strcmp(e.target, v) && strstr(data.data(), e.entry))
							target = e.target, entry = e.entry;
				}
				break;
			case 'g': target = "gs_4_0"; break;
			case 'h': target = "hs_5_0"; break;
			case 'd': target = "ds_5_0"; break;
			case 'c': target = "cs_5_0"; break;
			}
			t_skinning = job.skinning;
			void* unused = nullptr;
			const HRESULT hr = RImplementation.shader_compile(job.name.c_str(), (DWORD const*)data.data(), size, entry,
				target, D3D10_SHADER_PACK_MATRIX_ROW_MAJOR, unused);
			if (hr == S_FALSE)
				s_compiled++;
			else if (FAILED(hr))
				s_failed++;
		}
	}

	// The variant (settings digits and the sources' fingerprint) shader_compile names for a skinning mode
	static xr_string variant_of(int skinning)
	{
		t_variant_only = true;
		t_skinning = skinning;
		void* unused = nullptr;
		RImplementation.shader_compile("", nullptr, 0, "main", "vs_2_0", 0, unused);
		t_variant_only = false;
		t_skinning = INT_MIN;
		return t_variant;
	}

	// The jobs: each cache folder's skinning modes without a file of the current variant
	static void collect()
	{
		s_jobs.clear();
		const int modes[] = { -1, 0, 1, 2, 3, 4 };
		xr_string variants[6];
		for (int m = 0; m < 6; ++m)
			variants[m] = variant_of(modes[m]);
		// The digits telling the skinning modes apart
		xr_vector<size_t> skin_digits;
		for (size_t c = 0; c < variants[0].size(); ++c)
			for (int m = 1; m < 6; ++m)
				if (c < variants[m].size() && variants[m][c] != variants[0][c])
				{
					skin_digits.push_back(c);
					break;
				}
		const size_t digits = variants[0].find('_') == xr_string::npos ? variants[0].size() : variants[0].find('_');

		string_path root;
		FS.update_path(root, "$app_data_root$", "shaders_cache\\r4\\");
		WIN32_FIND_DATAA fd;
		HANDLE find = FindFirstFileA((xr_string(root) + "*").c_str(), &fd);
		if (find == INVALID_HANDLE_VALUE)
			return;
		do
		{
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.')
				continue;
			const xr_string folder = fd.cFileName;
			const size_t dot = folder.rfind('.');
			if (dot == xr_string::npos || folder.size() - dot != 3)
				continue;
			// Shaders patched in memory or with a material's options: their source isn't the file alone
			if (folder.find('(') != xr_string::npos || folder.find("_nxjit") != xr_string::npos ||
				folder.find("_nxlens") != xr_string::npos)
				continue;
			const char type = folder[dot + 1];
			const xr_string base = folder.substr(0, dot), ext = folder.substr(dot);
			// The skinning modes it was compiled with: its files' variants
			bool used[6] = {};
			WIN32_FIND_DATAA ff;
			HANDLE files = FindFirstFileA((xr_string(root) + folder + "\\*").c_str(), &ff);
			if (files == INVALID_HANDLE_VALUE)
				continue;
			do
			{
				if (ff.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					continue;
				const xr_string name = ff.cFileName;
				if (name.size() < digits)
					continue;
				for (int m = 0; m < 6; ++m)
				{
					bool match = true;
					for (size_t c : skin_digits)
						match = match && name[c] == variants[m][c];
					used[m] = used[m] || match;
				}
			} while (FindNextFileA(files, &ff));
			FindClose(files);
			for (int m = 0; m < 6; ++m)
			{
				if (!used[m])
					continue;
				Job job;
				job.type = type;
				job.skinning = modes[m];
				job.name = base;
				job.source = base + ext;
				// The vertex shaders of skinning modes 1..4 are named name_<mode>
				if (type == 'v' && modes[m] > 0)
				{
					string16 suffix;
					xr_sprintf(suffix, "_%d", modes[m]);
					if (base.size() > 2 && base.compare(base.size() - 2, 2, suffix) == 0)
						job.source = base.substr(0, base.size() - 2) + ext;
					else
						continue; // not this mode's name
				}
				else if (type == 'v' && base.size() > 2 && base[base.size() - 2] == '_' && isdigit(u8(base.back())) &&
					!FS.exist("$game_shaders$", (xr_string(::Render->getShaderPath()) + base + ext).c_str()))
					continue;
				string_path cached;
				xr_sprintf(cached, "%s%s\\%s", root, folder.c_str(), variants[m].c_str());
				if (GetFileAttributesA(cached) != INVALID_FILE_ATTRIBUTES)
					continue;
				s_jobs.push_back(job);
			}
		} while (FindNextFileA(find, &fd));
		FindClose(find);
	}

	static void join()
	{
		s_stop = true;
		for (std::thread& t : s_threads)
			if (t.joinable())
				t.join();
		s_threads.clear();
	}

	// Each frame: done, or new settings to compile for (variant: the last compiled shader's, its skinning mode's)
	void update(LPCSTR last_compiled)
	{
		static string_path s_last_seen = "";
		if (!last_compiled[0] || !xr_strcmp(s_last_seen, last_compiled))
		{
			if (s_threads.empty() || s_next < s_jobs.size())
				return;
		}
		xr_strcpy(s_last_seen, last_compiled);
		string_path variant = "";
		if (last_compiled[0])
			xr_strcpy(variant, variant_of(-1).c_str());
		if (!s_threads.empty() && s_next >= s_jobs.size())
		{
			join();
			shader_progress::end(true);
			Msg("* Shader pre-compilation: %u shaders compiled ahead in %.1f s (%u failed, %u already there)", u32(s_compiled),
				s_timer.GetElapsed_sec(), u32(s_failed), u32(s_jobs.size()) - s_compiled - s_failed);
		}
		if (!ps_r4_shader_precompile || !variant[0] || !s_threads.empty() || !xr_strcmp(s_started_variant, variant))
			return;
		xr_strcpy(s_started_variant, variant);
		s_timer.Start();
		collect();
		if (s_jobs.empty())
			return;
		s_stop = false;
		s_next = 0;
		s_compiled = s_failed = 0;
		const u32 cores = std::thread::hardware_concurrency();
		const u32 count = _min(u32(s_jobs.size()), cores > 3 ? cores - 2 : 1u);
		Msg("* Shader pre-compilation: %u shaders to compile for these settings (variant %s), %u threads", u32(s_jobs.size()),
			variant, count);
		shader_progress::begin("Compiling shaders for these settings", u32(s_jobs.size()));
		for (u32 i = 0; i < count; ++i)
			s_threads.emplace_back(worker);
	}

	void stop()
	{
		if (s_threads.empty())
			return;
		join();
		shader_progress::end(false);
		s_started_variant[0] = 0; // again after the device's restart
	}
}

void CRender::OnFrame()
{
	PROF_EVENT("CRender::OnFrame()");
	shader_warmup::start_once();
	shader_precompile::update(shader_warmup::s_variant);
	tex_streaming::update();
	
	Models->DeleteQueue();

	//Lights Delete queue
	for (light* L : v_all_lights_dque)
		xr_delete(L);
	v_all_lights_dque.clear();

	if (Details)
		g_pGamePersistent->GrassBendersUpdateAnimations();
}

// Particles
void CRender::ExportParticles()
{
	Msg("Export particles with R4");
	PSLibrary.Reload();
	PSLibrary.Save2();
}

void CRender::ImportParticles()
{
	Msg("Import particles with R4");
	PSLibrary.OnDestroy();
	PSLibrary.Load2();
	PSLibrary.Save();
	PSLibrary.Reload();
}


// Implementation
IRender_ObjectSpecific* CRender::ros_create(IRenderable* parent) { return xr_new<CROS_impl>(); }
void CRender::ros_destroy(IRender_ObjectSpecific* & p) { xr_delete(p); }
IRenderVisual* CRender::model_Create(LPCSTR name, IReader* data) { return Models->Create(name, data); }
IRenderVisual* CRender::model_CreateChild(LPCSTR name, IReader* data) { return Models->CreateChild(name, data); }
IRenderVisual* CRender::model_Duplicate(IRenderVisual* V) { return Models->Instance_Duplicate((dxRender_Visual*)V); }

void CRender::model_Delete(IRenderVisual*& V, BOOL bDiscard)
{
	dxRender_Visual* pVisual = (dxRender_Visual*)V;
    if (Models)
	    Models->Delete(pVisual, bDiscard);
	V = 0;
}

void CRender::model_Delete_Deffered(IRenderVisual*& V)
{
	dxRender_Visual* pVisual = (dxRender_Visual*)V;
	Models->DeleteDeffered(pVisual);
	V = 0;
}

IRender_DetailModel* CRender::model_CreateDM(IReader* F)
{
	CDetail* D = xr_new<CDetail>();
	D->Load(F);
	return D;
}

void CRender::model_Delete(IRender_DetailModel* & F)
{
	if (F)
	{
		CDetail* D = (CDetail*)F;
		D->Unload();
		xr_delete(D);
		F = NULL;
	}
}

IRenderVisual* CRender::model_CreatePE(LPCSTR name)
{
	PS::CPEDef* SE = PSLibrary.FindPED(name);
	R_ASSERT3(SE, "Particle effect doesn't exist", name);
	return Models->CreatePE(SE);
}

IRenderVisual* CRender::model_CreateParticles(LPCSTR name)
{
	PS::CPEDef* SE = PSLibrary.FindPED(name);
	if (SE) return Models->CreatePE(SE);
	else
	{
		PS::CPGDef* SG = PSLibrary.FindPGD(name);
		R_ASSERT3(SG, "Particle effect or group doesn't exist", name);
		return Models->CreatePG(SG);
	}
}

void CRender::models_Prefetch() { Models->Prefetch(); }
void CRender::models_PrefetchOne(LPCSTR name, bool assert) { Models->Prefetch_One(name, assert); }
void CRender::models_Clear(BOOL b_complete) { Models->ClearPool(b_complete); }
bool CRender::models_Exists(LPCSTR name, bool no_prefetch) { return Models->Exists(name, no_prefetch); }

ref_shader CRender::getShader(int id)
{
	VERIFY(id<int(Shaders.size()));
	return Shaders[id];
}

IRender_Portal* CRender::getPortal(int id)
{
	VERIFY(id<int(Portals.size()));
	return Portals[id];
}

IRender_Sector* CRender::getSector(int id)
{
	if (id >= 0 && id < int(Sectors.size()))
		return Sectors[id];

	return NULL;
}

IRender_Sector* CRender::getSectorActive() { return pLastSector; }

IRenderVisual* CRender::getVisual(int id)
{
	VERIFY(id<int(Visuals.size()));
	return Visuals[id];
}

D3DVERTEXELEMENT9* CRender::getVB_Format(int id, BOOL _alt)
{
	if (_alt)
	{
		VERIFY(id<int(xDC.size()));
		return xDC[id].begin();
	}
	else
	{
		VERIFY(id<int(nDC.size()));
		return nDC[id].begin();
	}
}

ID3DVertexBuffer* CRender::getVB(int id, BOOL _alt)
{
	if (_alt)
	{
		VERIFY(id<int(xVB.size()));
		return xVB[id];
	}
	else
	{
		VERIFY(id<int(nVB.size()));
		return nVB[id];
	}
}

ID3DIndexBuffer* CRender::getIB(int id, BOOL _alt)
{
	if (_alt)
	{
		VERIFY(id<int(xIB.size()));
		return xIB[id];
	}
	else
	{
		VERIFY(id<int(nIB.size()));
		return nIB[id];
	}
}

FSlideWindowItem* CRender::getSWI(int id)
{
	VERIFY(id<int(SWIs.size()));
	return &SWIs[id];
}

IRender_Target* CRender::getTarget() { return Target; }

IRender_Light* CRender::light_create() { return Lights.Create(); }
IRender_Glow* CRender::glow_create() { return xr_new<CGlow>(); }

void CRender::flush() { RImplementation.GMBase.r_dsgraph_render_graph(0); }

BOOL CRender::occ_visible(vis_data& P) { return HOM.visible(P); }
BOOL CRender::occ_visible(sPoly& P) { return HOM.visible(P); }
BOOL CRender::occ_visible(Fbox& P) { return HOM.visible(P); }

// demonized: add user defined rotation to wallmark
void CRender::add_StaticWallmark(ref_shader& S, const Fvector& P, float s, CDB::TRI* T, Fvector* verts, float ttl, bool ignore_opt, bool random_rotation)
{
	add_StaticWallmark(S, P, s, T, verts, ttl, ignore_opt, random_rotation ? ::Random.randF(-20.f, 20.f) : 0.f);
}

void CRender::add_StaticWallmark(ref_shader& S, const Fvector& P, float s, CDB::TRI* T, Fvector* verts, float ttl, bool ignore_opt, float rotation)
{
	if (T->suppress_wm) return;
	VERIFY2(_valid(P) && _valid(s) && T && verts && (s > EPS_L), "Invalid static wallmark params");
	Wallmarks->AddStaticWallmark(T, verts, P, &*S, s, ttl, ignore_opt, rotation);
}

void CRender::add_StaticWallmark(IWallMarkArray* pArray, const Fvector& P, float s, CDB::TRI* T, Fvector* V, float ttl, bool ignore_opt, bool random_rotation)
{
	add_StaticWallmark(pArray, P, s, T, V, ttl, ignore_opt, random_rotation ? ::Random.randF(-20.f, 20.f) : 0.f);
}

void CRender::add_StaticWallmark(IWallMarkArray* pArray, const Fvector& P, float s, CDB::TRI* T, Fvector* V, float ttl, bool ignore_opt, float rotation)
{
	dxWallMarkArray* pWMA = (dxWallMarkArray*)pArray;
	ref_shader* pShader = pWMA->dxGenerateWallmark();
	if (pShader) add_StaticWallmark(*pShader, P, s, T, V, ttl, ignore_opt, rotation);
}

void CRender::add_StaticWallmark(const wm_shader& S, const Fvector& P, float s, CDB::TRI* T, Fvector* V)
{
	dxUIShader* pShader = (dxUIShader*)&*S;
	add_StaticWallmark(pShader->hShader, P, s, T, V, 0.0f, false, true);
}

void CRender::clear_static_wallmarks()
{
	Wallmarks->clear();
}

void CRender::add_SkeletonWallmark(intrusive_ptr<CSkeletonWallmark> wm)
{
	Wallmarks->AddSkeletonWallmark(std::move(wm));
}

void CRender::add_SkeletonWallmark(const Fmatrix* xf, CKinematics* obj, ref_shader& sh, const Fvector& start,
                                   const Fvector& dir, float size, float ttl, bool ignore_opt)
{
	Wallmarks->AddSkeletonWallmark(xf, obj, sh, start, dir, size, ttl, ignore_opt);
}

void CRender::add_SkeletonWallmark(const Fmatrix* xf, IKinematics* obj, IWallMarkArray* pArray, const Fvector& start,
                                   const Fvector& dir, float size, float ttl, bool ignore_opt)
{
	dxWallMarkArray* pWMA = (dxWallMarkArray *)pArray;
	ref_shader* pShader = pWMA->dxGenerateWallmark();
	if (pShader) add_SkeletonWallmark(xf, (CKinematics*)obj, *pShader, start, dir, size, ttl, ignore_opt);
}

void CRender::remove_SkeletonWallmarksFromObject(IKinematics* obj)
{
    Wallmarks->RemoveSkeletonWallmarksFromObject(static_cast<CKinematics*>(obj));
}

void CRender::update_Wallmarks()
{
    Wallmarks->UpdateWallmarks();
}

void CRender::add_Occluder(Fbox2& bb_screenspace)
{
	HOM.occlude(bb_screenspace);
}

void CRender::rmNear()
{
	IRender_Target* T = getTarget();
	D3D_VIEWPORT VP = {0, 0, (float)T->get_width(), (float)T->get_height(), 0, 0.02f};

	HW.pContext->RSSetViewports(1, &VP);
	//CHK_DX				(HW.pDevice->SetViewport(&VP));
}

void CRender::rmFar()
{
	IRender_Target* T = getTarget();
	D3D_VIEWPORT VP = {0, 0, (float)T->get_width(), (float)T->get_height(), 0.99999f, 1.f};

	HW.pContext->RSSetViewports(1, &VP);
	//CHK_DX				(HW.pDevice->SetViewport(&VP));
}

void CRender::rmNormal()
{
	IRender_Target* T = getTarget();
	D3D_VIEWPORT VP = {0, 0, (float)T->get_width(), (float)T->get_height(), 0, 1.f};

	HW.pContext->RSSetViewports(1, &VP);
	//CHK_DX				(HW.pDevice->SetViewport(&VP));
}

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////
CRender::CRender()
	: m_bFirstFrameAfterReset(false), upscale_jitter_on(false), upscale_scene_active(false), upscale_pda_late(false)
{
	upscale_jitter_ndc.set(0.f, 0.f);
	upscale_jitter_prev_px.set(0.f, 0.f);
	upscale_lens_res.set(0.f, 0.f, 0.f, 0.f);
	init_cacades();
}

CRender::~CRender()
{
	for (auto& it : SWIs) {
		xr_free(it.sw);
		it.sw = nullptr;
		it.count = 0;
	}
	SWIs.clear();
}

#include "../../xrEngine/GameFont.h"

void CRender::Statistics(CGameFont* _F)
{
	CGameFont& F = *_F;
	F.OutNext(" **** LT:%2d,LV:%2d **** ", stats.l_total, stats.l_visible);
	stats.l_visible = 0;
	F.OutNext("    S(%2d)   | (%2d)NS   ", stats.l_shadowed, stats.l_unshadowed);
	F.OutNext("LP sh(in:%u,vis:%u,r:%u) pk(%u/%u)", stats.ls_shadowed_in, stats.ls_shadowed_after_vis,
	          stats.ls_shadowed_rendered, stats.ls_shadowed_peak_in, stats.ls_shadowed_peak_after_vis);
	F.OutNext("LP uns p(in:%u,r:%u) s(in:%u,r:%u)", stats.ls_unshadowed_point_in, stats.ls_unshadowed_point_rendered,
	          stats.ls_unshadowed_spot_in, stats.ls_unshadowed_spot_rendered);
	F.OutNext("LP sh skip: pend[%u] inv[%u]", stats.ls_shadowed_pending_skipped, stats.ls_shadowed_invisible_skipped);
	stats.ls_shadowed_in = 0;
	stats.ls_shadowed_after_vis = 0;
	stats.ls_shadowed_rendered = 0;
	stats.ls_shadowed_pending_skipped = 0;
	stats.ls_shadowed_invisible_skipped = 0;
	stats.ls_shadowed_peak_in = 0;
	stats.ls_shadowed_peak_after_vis = 0;
	stats.ls_unshadowed_point_in = 0;
	stats.ls_unshadowed_spot_in = 0;
	stats.ls_unshadowed_point_rendered = 0;
	stats.ls_unshadowed_spot_rendered = 0;
	F.OutNext("smap use[%2d], merge[%2d], finalclip[%2d]", stats.s_used, stats.s_merged - stats.s_used,
	          stats.s_finalclip);
	stats.s_used = 0;
	stats.s_merged = 0;
	stats.s_finalclip = 0;
	F.OutSkip();
	F.OutNext(" **** Occ-Q(%03.1f) **** ", 100.f * f32(stats.o_culled) / f32(stats.o_queries ? stats.o_queries : 1));
	F.OutNext(" total  : %2d", stats.o_queries);
	stats.o_queries = 0;
	F.OutNext(" culled : %2d", stats.o_culled);
	stats.o_culled = 0;
	F.OutSkip();
	u32 ict = stats.ic_total + stats.ic_culled;
	F.OutNext(" **** iCULL(%03.1f) **** ", 100.f * f32(stats.ic_culled) / f32(ict ? ict : 1));
	F.OutNext(" visible: %2d", stats.ic_total);
	stats.ic_total = 0;
	F.OutNext(" culled : %2d", stats.ic_culled);
	stats.ic_culled = 0;
#ifdef DEBUG
	HOM.stats	();
#endif
}

/////////
#pragma comment(lib,"d3dx9.lib")
/*
extern "C"
{
LPCSTR WINAPI	D3DXGetPixelShaderProfile	(LPDIRECT3DDEVICE9  pDevice);
LPCSTR WINAPI	D3DXGetVertexShaderProfile	(LPDIRECT3DDEVICE9	pDevice);
};
*/

void CRender::addShaderOption(const char* name, const char* value)
{
	D3D_SHADER_MACRO macro = {name, value};
	m_ShaderOptions.push_back(macro);
}


template <typename T>
static HRESULT create_shader(
	LPCSTR const pTarget,
	DWORD const* buffer,
	u32 const buffer_size,
	LPCSTR const file_name,
	T*& result,
	bool const disasm
)
{
	result->sh = ShaderTypeTraits<T>::CreateHWShader(buffer, buffer_size);
#ifdef USE_DX12
	if (result->sh)
		result->sh->m_name = file_name;
#endif

	ID3DShaderReflection* pReflection = 0;

	HRESULT const _hr = D3DReflect(buffer, buffer_size, IID_PPV_ARGS(&pReflection));
	if (SUCCEEDED(_hr) && pReflection)
	{
		// Parse constant table data
		result->constants.parse(pReflection, ShaderTypeTraits<T>::GetShaderDest());

		_RELEASE(pReflection);
	}
	else
	{
		Msg("! D3DReflectShader %s hr == 0x%08x", file_name, _hr);
	}

	return _hr;
}

static HRESULT create_shader(
	LPCSTR const pTarget,
	DWORD const* buffer,
	u32 const buffer_size,
	LPCSTR const file_name,
	void*& result,
	bool const disasm
)
{
	PROF_EVENT("create_shader");
	HRESULT _result = E_FAIL;
	if (pTarget[0] == 'p')
	{
		SPS* sps_result = (SPS*)result;
#ifdef USE_DX11
		_result = HW.pDevice->CreatePixelShader(buffer, buffer_size, 0, &sps_result->ps);
#ifdef USE_DX12
		if (SUCCEEDED(_result) && sps_result->ps)
			sps_result->ps->m_name = file_name;
#endif
#else // #ifdef USE_DX11
		_result			= HW.pDevice->CreatePixelShader(buffer, buffer_size, &sps_result->ps);
#endif // #ifdef USE_DX11
		if (!SUCCEEDED(_result))
		{
			Log("! PS: ", file_name);
			Msg("! CreatePixelShader hr == 0x%08x", _result);
			return E_FAIL;
		}

		ID3DShaderReflection* pReflection = 0;

#ifdef USE_DX11
        _result = D3DReflect(buffer, buffer_size, IID_PPV_ARGS(&pReflection));
#else
		_result			= D3D10ReflectShader( buffer, buffer_size, &pReflection);
#endif

		//	Parse constant, texture, sampler binding
		//	Store input signature blob
		if (SUCCEEDED(_result) && pReflection)
		{
			//	Let constant table parse it's data
			sps_result->constants.parse(pReflection, RC_dest_pixel);

			_RELEASE(pReflection);
		}
		else
		{
			Log("! PS: ", file_name);
			Msg("! D3DReflectShader hr == 0x%08x", _result);
		}
	}
	else if (pTarget[0] == 'v')
	{
		SVS* svs_result = (SVS*)result;
#ifdef USE_DX11
		_result = HW.pDevice->CreateVertexShader(buffer, buffer_size, 0, &svs_result->vs);
#ifdef USE_DX12
		if (SUCCEEDED(_result) && svs_result->vs)
			svs_result->vs->m_name = file_name;
#endif
#else // #ifdef USE_DX11
		_result			= HW.pDevice->CreateVertexShader(buffer, buffer_size, &svs_result->vs);
#endif // #ifdef USE_DX11

		if (!SUCCEEDED(_result))
		{
			Log("! VS: ", file_name);
			Msg("! CreatePixelShader hr == 0x%08x", _result);
			return E_FAIL;
		}

		ID3DShaderReflection* pReflection = 0;
#ifdef USE_DX11
        _result = D3DReflect(buffer, buffer_size, IID_PPV_ARGS(&pReflection));
#else
		_result			= D3D10ReflectShader( buffer, buffer_size, &pReflection);
#endif

		//	Parse constant, texture, sampler binding
		//	Store input signature blob
		if (SUCCEEDED(_result) && pReflection)
		{
			//	TODO: DX10: share the same input signatures

			//	Store input signature (need only for VS)
			//CHK_DX( D3DxxGetInputSignatureBlob(pShaderBuf->GetBufferPointer(), pShaderBuf->GetBufferSize(), &_vs->signature) );
			ID3DBlob* pSignatureBlob;
			CHK_DX(D3DGetInputSignatureBlob(buffer, buffer_size, &pSignatureBlob));
			VERIFY(pSignatureBlob);

			svs_result->signature = dxRenderDeviceRender::Instance().Resources->_CreateInputSignature(pSignatureBlob);

			_RELEASE(pSignatureBlob);

			//	Let constant table parse it's data
			svs_result->constants.parse(pReflection, RC_dest_vertex);

			_RELEASE(pReflection);
		}
		else
		{
			Log("! VS: ", file_name);
			Msg("! D3DXFindShaderComment hr == 0x%08x", _result);
		}
	}
	else if (pTarget[0] == 'g')
	{
		SGS* sgs_result = (SGS*)result;
#ifdef USE_DX11
		_result = HW.pDevice->CreateGeometryShader(buffer, buffer_size, 0, &sgs_result->gs);
#ifdef USE_DX12
		if (SUCCEEDED(_result) && sgs_result->gs)
			sgs_result->gs->m_name = file_name;
#endif
#else // #ifdef USE_DX11
		_result			= HW.pDevice->CreateGeometryShader(buffer, buffer_size, &sgs_result->gs);
#endif // #ifdef USE_DX11
		if (!SUCCEEDED(_result))
		{
			Log("! GS: ", file_name);
			Msg("! CreateGeometryShaderhr == 0x%08x", _result);
			return E_FAIL;
		}

		ID3DShaderReflection* pReflection = 0;

#ifdef USE_DX11
        _result = D3DReflect(buffer, buffer_size, IID_PPV_ARGS(&pReflection));
#else
		_result			= D3D10ReflectShader( buffer, buffer_size, &pReflection);
#endif

		//	Parse constant, texture, sampler binding
		//	Store input signature blob
		if (SUCCEEDED(_result) && pReflection)
		{
			//	Let constant table parse it's data
			sgs_result->constants.parse(pReflection, RC_dest_geometry);

			_RELEASE(pReflection);
		}
		else
		{
			Log("! PS: ", file_name);
			Msg("! D3DReflectShader hr == 0x%08x", _result);
		}
	}
		//	else if (pTarget[0] == 'c') {
		//		SCS* scs_result = (SCS*)result;
		//#ifdef USE_DX11
		//		_result			= HW.pDevice->CreateComputeShader(buffer, buffer_size, 0, &scs_result->sh);
		//#else // #ifdef USE_DX11
		//		_result			= HW.pDevice->CreateComputeShader(buffer, buffer_size, &scs_result->sh);
		//#endif // #ifdef USE_DX11
		//		if ( !SUCCEEDED(_result) ) {
		//			Log			("! CS: ", file_name);
		//			Msg			("! CreateComputeShaderhr == 0x%08x", _result);
		//			return		E_FAIL;
		//		}
		//
		//		ID3DShaderReflection *pReflection = 0;
		//
		//#ifdef USE_DX11
		//		_result			= D3DReflect( buffer, buffer_size, IID_ID3DShaderReflection, (void**)&pReflection);
		//#else
		//		_result			= D3D10ReflectShader( buffer, buffer_size, &pReflection);
		//#endif
		//
		//		//	Parse constant, texture, sampler binding
		//		//	Store input signature blob
		//		if (SUCCEEDED(_result) && pReflection)
		//		{
		//			//	Let constant table parse it's data
		//			scs_result->constants.parse(pReflection,RC_dest_pixel);
		//
		//			_RELEASE(pReflection);
		//		}
		//		else
		//		{
		//			Log	("! PS: ", file_name);
		//			Msg	("! D3DReflectShader hr == 0x%08x", _result);
		//		}
		//	}
	else if (pTarget[0] == 'c')
	{
		_result = create_shader(pTarget, buffer, buffer_size, file_name, (SCS*&)result, disasm);
	}
	else if (pTarget[0] == 'h')
	{
		_result = create_shader(pTarget, buffer, buffer_size, file_name, (SHS*&)result, disasm);
	}
	else if (pTarget[0] == 'd')
	{
		_result = create_shader(pTarget, buffer, buffer_size, file_name, (SDS*&)result, disasm);
	}
	else
	{
		NODEFAULT;
	}

	if (disasm)
	{
		ID3DBlob* disasm = 0;
		D3DDisassemble(buffer, buffer_size, FALSE, 0, &disasm);
		//D3DXDisassembleShader		(LPDWORD(code->GetBufferPointer()), FALSE, 0, &disasm );
		string_path dname;
		strconcat(sizeof(dname), dname, "disasm\\", file_name,
		          ('v' == pTarget[0]) ? ".vs" : ('p' == pTarget[0]) ? ".ps" : ".gs");
		IWriter* W = FS.w_open("$logs$", dname);
		W->w(disasm->GetBufferPointer(), (u32)disasm->GetBufferSize());
		FS.w_close(W);
		_RELEASE(disasm);
	}

	return _result;
}

//--------------------------------------------------------------------------------------------------------------
class includer : public ID3DInclude
{
public:
	HRESULT __stdcall Open(D3D10_INCLUDE_TYPE IncludeType, LPCSTR pFileName, LPCVOID pParentData, LPCVOID* ppData,
	                       UINT* pBytes)
	{
		string_path pname;
		strconcat(sizeof(pname), pname, ::Render->getShaderPath(), pFileName);
		IReader* R = shader_patches::open(pname);
		if (0 == R)
		{
			// possibly in shared directory or somewhere else - open directly
			R = shader_patches::open(pFileName);
			if (0 == R) return E_FAIL;
		}

		// duplicate and zero-terminate
		u32 size = R->length();
		u8* data = xr_alloc<u8>(size + 1);
		CopyMemory(data, R->pointer(), size);
		data[size] = 0;
		FS.r_close(R);

		*ppData = data;
		*pBytes = size;
		return D3D_OK;
	}

	HRESULT __stdcall Close(LPCVOID pData)
	{
		xr_free(pData);
		return D3D_OK;
	}
};

static inline bool match_shader_id(LPCSTR const debug_shader_id, LPCSTR const full_shader_id,
                                   FS_FileSet const& file_set, string_path& result);

// The compiled shader cache (appdata\shaders_cache) was keyed by the shader's name and the settings only: a shader
// mod that changed, was added or was turned off kept loading the old compiled code until the cache was deleted by hand.
// Its file names now also carry a fingerprint of every shader source file (name, size, time), taken once per session:
// any change to the shaders compiles them anew, once
static LPCSTR shader_cache_suffix()
{
	static string16 suffix = "";
	if (!suffix[0])
	{
		u32 crc = 0;
		FS_FileSet files;
		FS.file_list(files, "$game_shaders$", FS_ListFiles, "*");
		for (const FS_File& f : files)
		{
			crc = crc32(f.name.c_str(), u32(f.name.size()), crc);
			const s64 size = f.size, time = s64(f.time_write);
			crc = crc32(&size, sizeof(size), crc);
			crc = crc32(&time, sizeof(time), crc);
		}
		xr_sprintf(suffix, "_%08x", crc);
		Msg("* Shader sources: %u files, cache fingerprint %s", u32(files.size()), suffix + 1);
	}
	return suffix;
}

HRESULT CRender::shader_compile(
	LPCSTR name,
	DWORD const* pSrcData,
	UINT SrcDataLen,
	LPCSTR pFunctionName,
	LPCSTR pTarget,
	DWORD Flags,
	void*& result)
{
	D3D_SHADER_MACRO defines [128];
	int def_it = 0;
	char c_smapsize [32];
	char c_sun_shafts [32];
	char c_ssao [32];
	char c_sun_quality [32];
	char c_smaa_quality [32];
	
	// SSS preprocessor stuff
	char c_ssfx_sss_dir_quality[32];
	char c_ssfx_sss_omni_quality[32];
	char c_ssfx_terrain_pom_refine[32];
	char c_ssfx_pom_refine[32];
	char c_ssfx_il[32];
	char c_ssfx_ao[32];
	char c_ssfx_water[32];
	char c_ssfx_water_parallax[32];
	char c_ssr_quality[32];
	char c_rain_quality[32];
	char c_inter_grass[32];
	char c_shader_bus[32];

	char sh_name[MAX_PATH] = "";

	for (u32 i = 0; i < m_ShaderOptions.size(); ++i)
	{
		defines[def_it++] = m_ShaderOptions[i];
	}

	u32 len = xr_strlen(sh_name);
	// options
	const int m_skinning = shader_precompile::t_skinning != INT_MIN ? shader_precompile::t_skinning :
		Engine.External.GetSkinningMode();
	{
		xr_sprintf(c_smapsize, "%04d", u32(o.smapsize));
		defines[def_it].Name = "SMAP_size";
		defines[def_it].Definition = c_smapsize;
		def_it ++;
		VERIFY(xr_strlen(c_smapsize) == 4);
		xr_strcat(sh_name, c_smapsize);
		len += 4;
	}

	if (o.fp16_filter)
	{
		defines[def_it].Name = "FP16_FILTER";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(o.fp16_filter);
	++len;

	if (o.fp16_blend)
	{
		defines[def_it].Name = "FP16_BLEND";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(o.fp16_blend);
	++len;

	if (o.HW_smap)
	{
		defines[def_it].Name = "USE_HWSMAP";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(o.HW_smap);
	++len;

	if (o.HW_smap_PCF)
	{
		defines[def_it].Name = "USE_HWSMAP_PCF";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(o.HW_smap_PCF);
	++len;

	if (o.HW_smap_FETCH4)
	{
		defines[def_it].Name = "USE_FETCH4";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(o.HW_smap_FETCH4);
	++len;

	if (o.sjitter)
	{
		defines[def_it].Name = "USE_SJITTER";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(o.sjitter);
	++len;

	if (HW.Caps.raster_major >= 3)
	{
		defines[def_it].Name = "USE_BRANCHING";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(HW.Caps.raster_major >= 3);
	++len;

	if (HW.Caps.geometry.bVTF)
	{
		defines[def_it].Name = "USE_VTF";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(HW.Caps.geometry.bVTF);
	++len;

	if (o.Tshadows)
	{
		defines[def_it].Name = "USE_TSHADOWS";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(o.Tshadows);
	++len;

	if (ps_r2_anomaly_flags.test(R2_AN_FLAG_MBLUR))
	{
		defines[def_it].Name = "USE_MBLUR";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(ps_r2_anomaly_flags.test(R2_AN_FLAG_MBLUR));
	++len;

	if (o.sunfilter)
	{
		defines[def_it].Name = "USE_SUNFILTER";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(o.sunfilter);
	++len;

	if (o.sunstatic)
	{
		defines[def_it].Name = "USE_R2_STATIC_SUN";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(o.sunstatic);
	++len;

	if (o.forceskinw)
	{
		defines[def_it].Name = "SKIN_COLOR";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(o.forceskinw);
	++len;

	if (o.ssao_blur_on)
	{
		defines[def_it].Name = "USE_SSAO_BLUR";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(o.ssao_blur_on);
	++len;

	if (o.ssao_hdao)
	{
		defines[def_it].Name = "HDAO";
		defines[def_it].Definition = "1";
		def_it ++;
		sh_name[len] = '1';
		++len;
		sh_name[len] = '0';
		++len;
		sh_name[len] = '0';
		++len;
	}
	else
	{
		sh_name[len] = '0';
		++len;
		sh_name[len] = '0' + char(o.ssao_hbao);
		++len;
		sh_name[len] = '0' + char(o.ssao_half_data);
		++len;
		if (o.ssao_hbao)
		{
			defines[def_it].Name = "SSAO_OPT_DATA";
			if (o.ssao_half_data)
			{
				defines[def_it].Definition = "2";
			}
			else
			{
				defines[def_it].Definition = "1";
			}
			def_it ++;

			if (o.hbao_vectorized)
			{
				defines[def_it].Name = "VECTORIZED_CODE";
				defines[def_it].Definition = "1";
				def_it ++;
			}

			defines[def_it].Name = "USE_HBAO";
			defines[def_it].Definition = "1";
			def_it ++;
		}
	}

	if (o.dx10_msaa)
	{
		static char def[ 256 ];
		//if( m_MSAASample < 0 )
		//{
		def[0] = '0';
		//	sh_name[len]='0'; ++len;
		//}
		//else
		//{
		//	def[0]= '0' + char(m_MSAASample);
		//	sh_name[len]='0' + char(m_MSAASample); ++len;
		//}
		def[1] = 0;
		defines[def_it].Name = "ISAMPLE";
		defines[def_it].Definition = def;
		def_it ++;
		sh_name[len] = '0';
		++len;
	}
	else
	{
		sh_name[len] = '0';
		++len;
	}

	// skinning
	if (m_skinning < 0)
	{
		defines[def_it].Name = "SKIN_NONE";
		defines[def_it].Definition = "1";
		def_it ++;
		sh_name[len] = '1';
		++len;
	}
	else
	{
		sh_name[len] = '0';
		++len;
	}

	if (0 == m_skinning)
	{
		defines[def_it].Name = "SKIN_0";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(0 == m_skinning);
	++len;

	if (1 == m_skinning)
	{
		defines[def_it].Name = "SKIN_1";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(1 == m_skinning);
	++len;

	if (2 == m_skinning)
	{
		defines[def_it].Name = "SKIN_2";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(2 == m_skinning);
	++len;

	if (3 == m_skinning)
	{
		defines[def_it].Name = "SKIN_3";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(3 == m_skinning);
	++len;

	if (4 == m_skinning)
	{
		defines[def_it].Name = "SKIN_4";
		defines[def_it].Definition = "1";
		def_it ++;
	}
	sh_name[len] = '0' + char(4 == m_skinning);
	++len;

	//	Igor: need restart options
	if (RImplementation.o.advancedpp && ps_r2_ls_flags.test(R2FLAG_SOFT_WATER))
	{
		defines[def_it].Name = "USE_SOFT_WATER";
		defines[def_it].Definition = "1";
		def_it ++;
		sh_name[len] = '1';
		++len;
	}
	else
	{
		sh_name[len] = '0';
		++len;
	}

	if (RImplementation.o.advancedpp && ps_r2_ls_flags.test(R2FLAG_SOFT_PARTICLES))
	{
		defines[def_it].Name = "USE_SOFT_PARTICLES";
		defines[def_it].Definition = "1";
		def_it ++;
		sh_name[len] = '1';
		++len;
	}
	else
	{
		sh_name[len] = '0';
		++len;
	}

	if (RImplementation.o.advancedpp && ps_r2_ls_flags.test(R2FLAG_DOF))
	{
		defines[def_it].Name = "USE_DOF";
		defines[def_it].Definition = "1";
		def_it ++;
		sh_name[len] = '1';
		++len;
	}
	else
	{
		sh_name[len] = '0';
		++len;
	}

	if (RImplementation.o.advancedpp && ps_sunshafts_mode)
	{
		xr_sprintf(c_sun_shafts, "%d", ps_r_sun_shafts);
		defines[def_it].Name = "SUN_SHAFTS_QUALITY";
		defines[def_it].Definition = c_sun_shafts;
		def_it ++;
		sh_name[len] = '0' + char(ps_r_sun_shafts);
		++len;
	}
	else
	{
		sh_name[len] = '0';
		++len;
	}

	if (RImplementation.o.advancedpp && ps_r_ssao)
	{
		xr_sprintf(c_ssao, "%d", ps_r_ssao);
		defines[def_it].Name = "SSAO_QUALITY";
		defines[def_it].Definition = c_ssao;
		def_it ++;
		sh_name[len] = '0' + char(ps_r_ssao);
		++len;
	}
	else
	{
		sh_name[len] = '0';
		++len;
	}

	if (RImplementation.o.advancedpp && ps_r_sun_quality)
	{
		xr_sprintf(c_sun_quality, "%d", ps_r_sun_quality);
		defines[def_it].Name = "SUN_QUALITY";
		defines[def_it].Definition = c_sun_quality;
		def_it ++;
		sh_name[len] = '0' + char(ps_r_sun_quality);
		++len;
	}
	else
	{
		sh_name[len] = '0';
		++len;
	}

	if (RImplementation.o.advancedpp && ps_r2_ls_flags.test(R2FLAG_STEEP_PARALLAX))
	{
		defines[def_it].Name = "ALLOW_STEEPPARALLAX";
		defines[def_it].Definition = "1";
		def_it ++;
		sh_name[len] = '1';
		++len;
	}
	else
	{
		sh_name[len] = '0';
		++len;
	}

	//////////////////////////////////lvutner	

	if (ps_r2_anomaly_flags.test(R2_AN_FLAG_WATER_REFLECTIONS))
	{
		defines[def_it].Name = "USE_REFLECTIONS";
		defines[def_it].Definition = "1";
		def_it ++;
		sh_name[len] = '1';
		++len;
	}
	else
	{
		sh_name[len] = '0';
		++len;
	}

	if (ps_smaa_quality)
	{
		xr_sprintf(c_smaa_quality, "%d", ps_smaa_quality);
		defines[def_it].Name = "SMAA_QUALITY";
		defines[def_it].Definition = c_smaa_quality;
		def_it++;
		sh_name[len] = '0' + char(ps_smaa_quality);
		++len;
	}
	else
	{
		sh_name[len] = '0';
		++len;
	}

	//Useful shit. 
	if (HW.Caps.id_vendor == 0x1002) //AMD hardware
	{
		defines[def_it].Name = "INT_RENDER_AMD";
		defines[def_it].Definition = "1";
		def_it++;
	}

	if (HW.Caps.id_vendor == 0x10DE) //NVidia hardware
	{
		defines[def_it].Name = "INT_RENDER_NVIDIA";
		defines[def_it].Definition = "1";
		def_it++;
	}
	
	/////////////////////////////////////////////


	//R_ASSERT						( !o.dx10_sm4_1 );
	if (o.dx10_sm4_1)
	{
		defines[def_it].Name = "SM_4_1";
		defines[def_it].Definition = "1";
		def_it++;
	}
	sh_name[len] = '0' + char(o.dx10_sm4_1);
	++len;

	R_ASSERT(HW.FeatureLevel>=D3D_FEATURE_LEVEL_11_0);
	if (HW.FeatureLevel >= D3D_FEATURE_LEVEL_11_0)
	{
		defines[def_it].Name = "SM_5";
		defines[def_it].Definition = "1";
		def_it++;
	}
	sh_name[len] = '0' + char(HW.FeatureLevel >= D3D_FEATURE_LEVEL_11_0);
	++len;

	if (o.dx10_minmax_sm)
	{
		defines[def_it].Name = "USE_MINMAX_SM";
		defines[def_it].Definition = "1";
		def_it++;
	}
	sh_name[len] = '0' + char(o.dx10_minmax_sm != 0);
	++len;
	
	if (ps_ssfx_rain_1.w > 0)
	{
		xr_sprintf(c_rain_quality, "%d", u8(ps_ssfx_rain_1.w));
		defines[def_it].Name = "SSFX_RAIN_QUALITY";
		defines[def_it].Definition = c_rain_quality;
		def_it++;
		xr_strcat(sh_name, c_rain_quality);
		len += xr_strlen(c_rain_quality);
	}
	else
	{
		sh_name[len] = '0';
		++len;
	}

	if (ps_ssfx_grass_interactive.y > 0)
	{
		xr_sprintf(c_inter_grass, "%d", u8(ps_ssfx_grass_interactive.y));
		defines[def_it].Name = "SSFX_INT_GRASS";
		defines[def_it].Definition = c_inter_grass;
		def_it++;
		xr_strcat(sh_name, c_inter_grass);
		len += xr_strlen(c_inter_grass);
	}
	else
	{
		sh_name[len] = '0';
		++len;
	}

	xr_sprintf(c_ssr_quality, "%d", u8(std::min(std::max(ps_ssfx_ssr_quality, 0), 5)));
	defines[def_it].Name = "SSFX_SSR_QUALITY";
	defines[def_it].Definition = c_ssr_quality;
	def_it++;
	xr_strcat(sh_name, c_ssr_quality);
	len += xr_strlen(c_ssr_quality);

	xr_sprintf(c_ssfx_water, "%d", u8(std::min(std::max(ps_ssfx_water_quality.x, 0.0f), 4.0f)));
	defines[def_it].Name = "SSFX_WATER_QUALITY";
	defines[def_it].Definition = c_ssfx_water;
	def_it++;
	xr_strcat(sh_name, c_ssfx_water);
	len += xr_strlen(c_ssfx_water);

	xr_sprintf(c_ssfx_water_parallax, "%d", u8(std::min(std::max(ps_ssfx_water_quality.y, 0.0f), 3.0f)));
	defines[def_it].Name = "SSFX_WATER_PARALLAX";
	defines[def_it].Definition = c_ssfx_water_parallax;
	def_it++;
	xr_strcat(sh_name, c_ssfx_water_parallax);
	len += xr_strlen(c_ssfx_water_parallax);

	xr_sprintf(c_ssfx_il, "%d", u8(std::min(std::max(ps_ssfx_il_quality, 0), 64)));
	defines[def_it].Name = "SSFX_IL_QUALITY";
	defines[def_it].Definition = c_ssfx_il;
	def_it++;
	xr_strcat(sh_name, c_ssfx_il);
	len += xr_strlen(c_ssfx_il);

	xr_sprintf(c_ssfx_ao, "%d", u8(std::min(std::max(ps_ssfx_ao_quality, 2), 8)));
	defines[def_it].Name = "SSFX_AO_QUALITY";
	defines[def_it].Definition = c_ssfx_ao;
	def_it++;
	xr_strcat(sh_name, c_ssfx_ao);
	len += xr_strlen(c_ssfx_ao);

	xr_sprintf(c_ssfx_pom_refine, "%d", u8(std::min(std::max(ps_ssfx_pom_refine, 0), 1)));
	// The shaders test #ifdef: defined only when on, so 0 turns the refine off as the setting says (it ran always)
	if (c_ssfx_pom_refine[0] == '1')
	{
		defines[def_it].Name = "SSFX_POM_REFINE";
		defines[def_it].Definition = c_ssfx_pom_refine;
		def_it++;
	}
	xr_strcat(sh_name, c_ssfx_pom_refine);
	len += xr_strlen(c_ssfx_pom_refine);

	xr_sprintf(c_ssfx_terrain_pom_refine, "%d", u8(std::min(std::max(ps_ssfx_terrain_pom_refine, 0), 1)));
	// The shaders test #ifdef: defined only when on, so 0 turns the refine off as the setting says (it ran always)
	if (c_ssfx_terrain_pom_refine[0] == '1')
	{
		defines[def_it].Name = "SSFX_TERRA_POM_REFINE";
		defines[def_it].Definition = c_ssfx_terrain_pom_refine;
		def_it++;
	}
	xr_strcat(sh_name, c_ssfx_terrain_pom_refine);
	len += xr_strlen(c_ssfx_terrain_pom_refine);

	xr_sprintf(c_ssfx_sss_dir_quality, "%d", u8(std::min(std::max((int)ps_ssfx_sss_quality.x, 1), 24)));
	defines[def_it].Name = "SSFX_SSS_DIR_QUALITY";
	defines[def_it].Definition = c_ssfx_sss_dir_quality;
	def_it++;
	xr_strcat(sh_name, c_ssfx_sss_dir_quality);
	len += xr_strlen(c_ssfx_sss_dir_quality);

	xr_sprintf(c_ssfx_sss_omni_quality, "%d", u8(std::min(std::max((int)ps_ssfx_sss_quality.y, 1), 12)));
	defines[def_it].Name = "SSFX_SSS_OMNI_QUALITY";
	defines[def_it].Definition = c_ssfx_sss_omni_quality;
	def_it++;
	xr_strcat(sh_name, c_ssfx_sss_omni_quality);
	len += xr_strlen(c_ssfx_sss_omni_quality);

	defines[def_it].Name = "SSFX_MODEXE";
	defines[def_it].Definition = "1";
	def_it++;
	sh_name[len] = '1';
	++len;

	//Be carefull!!!!! this should be at the end to correctly generate
	//compiled shader name;
	// add a #define for DX10_1 MSAA support
	if (o.dx10_msaa)
	{
		defines[def_it].Name = "USE_MSAA";
		defines[def_it].Definition = "1";
		def_it ++;
		sh_name[len] = '1';
		++len;

		static char samples[2];

		defines[def_it].Name = "MSAA_SAMPLES";
		samples[0] = char(o.dx10_msaa_samples) + '0';
		samples[1] = 0;
		defines[def_it].Definition = samples;
		def_it ++;
		sh_name[len] = '0' + char(o.dx10_msaa_samples);
		++len;

		if (o.dx10_msaa_opt)
		{
			defines[def_it].Name = "MSAA_OPTIMIZATION";
			defines[def_it].Definition = "1";
			def_it ++;
		}
		sh_name[len] = '0' + char(o.dx10_msaa_opt);
		++len;

		switch (o.dx10_msaa_alphatest)
		{
		case MSAA_ATEST_DX10_0_ATOC:
			defines[def_it].Name = "MSAA_ALPHATEST_DX10_0_ATOC";
			defines[def_it].Definition = "1";
			def_it ++;
			sh_name[len] = '1';
			++len;
			sh_name[len] = '0';
			++len;
			sh_name[len] = '0';
			++len;
			break;
		case MSAA_ATEST_DX10_1_ATOC:
			defines[def_it].Name = "MSAA_ALPHATEST_DX10_1_ATOC";
			defines[def_it].Definition = "1";
			def_it ++;
			sh_name[len] = '0';
			++len;
			sh_name[len] = '1';
			++len;
			sh_name[len] = '0';
			++len;
			break;
		case MSAA_ATEST_DX10_1_NATIVE:
			defines[def_it].Name = "MSAA_ALPHATEST_DX10_1";
			defines[def_it].Definition = "1";
			def_it ++;
			sh_name[len] = '0';
			++len;
			sh_name[len] = '0';
			++len;
			sh_name[len] = '1';
			++len;
			break;
		default:
			sh_name[len] = '0';
			++len;
			sh_name[len] = '0';
			++len;
			sh_name[len] = '0';
			++len;
		}
	}
	else
	{
		sh_name[len] = '0';
		++len;
		sh_name[len] = '0';
		++len;
		sh_name[len] = '0';
		++len;
		sh_name[len] = '0';
		++len;
		sh_name[len] = '0';
		++len;
		sh_name[len] = '0';
		++len;
	}

	// NEXTRAY_SDR: the scene's color buffers are 8 bit (0..1), not HDR10's 16 bit floats; shaders may rely on the
	// range (the neXt-Ray patch mod's fog scattering early-out). In the variant name: compiled code differs
	if (!o.dx11_hdr10)
	{
		defines[def_it].Name = "NEXTRAY_SDR";
		defines[def_it].Definition = "1";
		def_it++;
	}
	sh_name[len] = '0' + char(!o.dx11_hdr10);
	++len;

	// NEXTRAY_HDR_SCENE: HDR output's scene highlights, combine_2 writes the scene's brightness (SV_Target1)
	if (o.nextray_hdr_scene)
	{
		defines[def_it].Name = "NEXTRAY_HDR_SCENE";
		defines[def_it].Definition = "1";
		def_it++;
	}
	sh_name[len] = '0' + char(o.nextray_hdr_scene);
	++len;

	// NEXTRAY_PATCHES: the bits of r4_shader_patches, three hex digits in the variant name
	char c_nextray_patches[8];
	xr_sprintf(c_nextray_patches, "0x%03X", u32(ps_r4_shader_patches & 0xFFF));
	defines[def_it].Name = "NEXTRAY_PATCHES";
	defines[def_it].Definition = c_nextray_patches;
	def_it++;
	for (int i = 2; i < 5; ++i)
	{
		sh_name[len] = c_nextray_patches[i];
		++len;
	}

	// the bus version, in the cache name too so each version keeps its own build
	xr_sprintf(c_shader_bus, "%d", ShaderBus::version());
	defines[def_it].Name = "SHADER_BUS";
	defines[def_it].Definition = c_shader_bus;
	def_it++;
	xr_strcat(sh_name, c_shader_bus);
	len += xr_strlen(c_shader_bus);

	sh_name[len] = 0;
	{
		string_path cached;
		xr_strcpy(cached, sh_name);
		xr_strcat(cached, shader_cache_suffix());
		if (shader_precompile::t_variant_only)
		{
			xr_strcpy(shader_precompile::t_variant, cached);
			return S_OK;
		}
		if (shader_precompile::t_skinning == INT_MIN)
			shader_warmup::note_variant(cached);
	}

	// finish
	defines[def_it].Name = 0;
	defines[def_it].Definition = 0;
	def_it ++;

	// 
	if (0 == xr_strcmp(pFunctionName, "main"))
	{
		if ('v' == pTarget[0])
		{
			if (HW.FeatureLevel == D3D_FEATURE_LEVEL_10_0)
				pTarget = "vs_4_0";
			else if (HW.FeatureLevel == D3D_FEATURE_LEVEL_10_1)
				pTarget = "vs_4_1";
			else if (HW.FeatureLevel == D3D_FEATURE_LEVEL_11_0)
				pTarget = "vs_5_0";
		}
		else if ('p' == pTarget[0])
		{
			if (HW.FeatureLevel == D3D_FEATURE_LEVEL_10_0)
				pTarget = "ps_4_0";
			else if (HW.FeatureLevel == D3D_FEATURE_LEVEL_10_1)
				pTarget = "ps_4_1";
			else if (HW.FeatureLevel == D3D_FEATURE_LEVEL_11_0)
				pTarget = "ps_5_0";
		}
		else if ('g' == pTarget[0])
		{
			if (HW.FeatureLevel == D3D_FEATURE_LEVEL_10_0)
				pTarget = "gs_4_0";
			else if (HW.FeatureLevel == D3D_FEATURE_LEVEL_10_1)
				pTarget = "gs_4_1";
			else if (HW.FeatureLevel == D3D_FEATURE_LEVEL_11_0)
				pTarget = "gs_5_0";
		}
		else if ('c' == pTarget[0])
		{
			if (HW.FeatureLevel == D3D_FEATURE_LEVEL_11_0)
				pTarget = "cs_5_0";
		}
	}

	HRESULT _result = E_FAIL;

	string_path folder_name, folder;
	xr_strcpy(folder, "r3\\objects\\r4\\");
	xr_strcat(folder, name);
	xr_strcat(folder, ".");

	char extension[3];
	strncpy_s(extension, pTarget, 2);
	xr_strcat(folder, extension);

	FS.update_path(folder_name, "$game_shaders$", folder);
	xr_strcat(folder_name, "\\");

	FS_FileSet file_set; // not the member: the pre-compilation threads compile too
	FS.file_list(file_set, folder_name, FS_ListFiles | FS_RootOnly, "*");

	string_path temp_file_name, file_name;
	bool const useGeneratedShaderCache =
		psDeviceFlags2.test(rsPrecompiledShaders) || !match_shader_id(name, sh_name, file_set, temp_file_name);
	if (useGeneratedShaderCache)
	{
		string_path file;
		xr_strcpy(file, "shaders_cache\\r4\\");
		xr_strcat(file, name);
		xr_strcat(file, ".");
		xr_strcat(file, extension);
		xr_strcat(file, "\\");
		xr_strcat(file, sh_name);
		xr_strcat(file, shader_cache_suffix());
		FS.update_path(file_name, "$app_data_root$", file);
	}
	else
	{
		xr_strcpy(file_name, folder_name);
		xr_strcat(file_name, temp_file_name);
	}

	u32 source_crc = 0;
	if (useGeneratedShaderCache)
		source_crc = getShaderSourceCrc32(pSrcData, SrcDataLen, ::Render->getShaderPath());

	// Pre-compilation: the cache file only, S_FALSE when compiled
	if (shader_precompile::t_ahead)
	{
		if (FS.exist(file_name) || !shader_precompile::begin(file_name))
			return S_OK;
		includer Includer;
		LPD3DBLOB code = nullptr, errors = nullptr;
		_result = D3DCompile(pSrcData, SrcDataLen, "", defines, &Includer, pFunctionName, pTarget, Flags, 0, &code, &errors);
		if (SUCCEEDED(_result))
		{
			IWriter* file = FS.w_open(file_name);
			// The same layout as below (upstream's source CRC first in a generated cache file)
			if (useGeneratedShaderCache)
				file->w_u32(source_crc);
			file->w_u32(crc32(code->GetBufferPointer(), code->GetBufferSize()));
			file->w(code->GetBufferPointer(), (u32)code->GetBufferSize());
			FS.w_close(file);
			_result = S_FALSE;
		}
		else
			Msg("! Shader pre-compilation: %s: %s", file_name, errors ? (LPCSTR)errors->GetBufferPointer() : "failed");
		_RELEASE(code);
		_RELEASE(errors);
		shader_precompile::finish(file_name);
		return _result;
	}
	// A pre-compilation thread compiling this one: its result instead of a second compile
	if (!FS.exist(file_name))
	{
		CTimer wait;
		wait.Start();
		if (shader_precompile::wait_for(file_name))
			shader_progress::stall_wait(wait.GetElapsed_sec() * 1000.f);
	}

	if (FS.exist(file_name))
	{
		PROF_EVENT("shader_compile: from the cache");
		IReader* file = FS.r_open(file_name);
		if (useGeneratedShaderCache)
		{
			if (file->length() > 8)
			{
				u32 const saved_source_crc = file->r_u32();
				if (saved_source_crc == source_crc)
				{
					u32 const crc = file->r_u32();
					u32 const real_crc = crc32(file->pointer(), file->elapsed());

					if (real_crc == crc)
					{
						_result = create_shader(pTarget, (DWORD*)file->pointer(), file->elapsed(), file_name, result, o.disasm);
					}
				}
				else
				{
					Msg("~Shader cache source CRC mismatch for '%s': cached=0x%08x current=0x%08x, recompiling",
						name, saved_source_crc, source_crc);
				}
			}
		}
		else if (file->length() > 4)
		{
			u32 const crc = file->r_u32();

			u32 const real_crc = crc32(file->pointer(), file->elapsed());

			if (real_crc == crc)
			{
				_result = create_shader(pTarget, (DWORD*)file->pointer(), file->elapsed(), file_name, result, o.disasm);
			}
		}
		file->close();
	}

	if (FAILED(_result))
	{
		includer Includer;
		LPD3DBLOB pShaderBuf = NULL;
		LPD3DBLOB pErrorBuf = NULL;
		PROF_EVENT("shader_compile: D3DCompile");
		CTimer compile_time;
		compile_time.Start();
		_result =
			D3DCompile(
				pSrcData,
				SrcDataLen,
				"", //NULL, //LPCSTR pFileName,	//	NVPerfHUD bug workaround.
				defines, &Includer, pFunctionName,
				pTarget,
				Flags, 0,
				&pShaderBuf,
				&pErrorBuf
			);

		shader_progress::stall_compile(compile_time.GetElapsed_sec() * 1000.f);
		if (SUCCEEDED(_result))
		{
			shader_progress::compiled();
			IWriter* file = FS.w_open(file_name);

			u32 const crc = crc32(pShaderBuf->GetBufferPointer(), pShaderBuf->GetBufferSize());

			if (useGeneratedShaderCache)
				file->w_u32(source_crc);

			file->w_u32(crc);
			file->w(pShaderBuf->GetBufferPointer(), (u32)pShaderBuf->GetBufferSize());
			FS.w_close(file);

			_result = create_shader(pTarget, (DWORD*)pShaderBuf->GetBufferPointer(), (u32)pShaderBuf->GetBufferSize(),
			                        file_name, result, o.disasm);
		}
		else
		{
			//			Msg						( "! shader compilation failed" );
			Log("! ", file_name);
			if (pErrorBuf)
				Log("! error: ", (LPCSTR)pErrorBuf->GetBufferPointer());
			else
				Msg("Can't compile shader hr=0x%08x", _result);
		}
	}

	return _result;
}

static inline bool match_shader(LPCSTR const debug_shader_id, LPCSTR const full_shader_id, LPCSTR const mask,
                                size_t const mask_length)
{
	u32 const full_shader_id_length = xr_strlen(full_shader_id);
	R_ASSERT2(
		full_shader_id_length == mask_length,
		make_string(
			"bad cache for shader %s, [%s], [%s]",
			debug_shader_id,
			mask,
			full_shader_id
		)
	);
	char const* i = full_shader_id;
	char const* const e = full_shader_id + full_shader_id_length;
	char const* j = mask;
	for (; i != e; ++i, ++j)
	{
		if (*i == *j)
			continue;

		if (*j == '_')
			continue;

		return false;
	}

	return true;
}

static inline bool match_shader_id(LPCSTR const debug_shader_id, LPCSTR const full_shader_id,
                                   FS_FileSet const& file_set, string_path& result)
{
#if 0
	strcpy_s					( result, "" );
	return						false;
#else // #if 1
#ifdef DEBUG
	LPCSTR temp					= "";
	bool found					= false;
	FS_FileSet::const_iterator	i = file_set.begin();
	FS_FileSet::const_iterator	const e = file_set.end();
	for ( ; i != e; ++i ) {
		if ( match_shader(debug_shader_id, full_shader_id, (*i).name.c_str(), (*i).name.size() ) ) {
			VERIFY				( !found );
			found				= true;
			temp				= (*i).name.c_str();
		}
	}

	xr_strcpy					( result, temp );
	return						found;
#else // #ifdef DEBUG
	FS_FileSet::const_iterator i = file_set.begin();
	FS_FileSet::const_iterator const e = file_set.end();
	for (; i != e; ++i)
	{
		if (match_shader(debug_shader_id, full_shader_id, (*i).name.c_str(), (*i).name.size()))
		{
			xr_strcpy(result, (*i).name.c_str());
			return true;
		}
	}

	return false;
#endif // #ifdef DEBUG
#endif// #if 1
}
