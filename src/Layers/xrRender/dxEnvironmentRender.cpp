#include "stdafx.h"
#include "dxEnvironmentRender.h"

#include "dxRenderDeviceRender.h"

#include "../../xrEngine/environment.h"
#include "../../Layers/xrRender/ResourceManager.h"

#include "../../xrEngine/xr_efflensflare.h"

static Fmatrix mSky_prev = Fidentity;

//////////////////////////////////////////////////////////////////////////
// half box def
static Fvector3 hbox_verts[24] =
{
	{-1.f, -1.f, -1.f}, {-1.f, -1.01f, -1.f}, // down
	{1.f, -1.f, -1.f}, {1.f, -1.01f, -1.f}, // down
	{-1.f, -1.f, 1.f}, {-1.f, -1.01f, 1.f}, // down
	{1.f, -1.f, 1.f}, {1.f, -1.01f, 1.f}, // down
	/* AVO: sky box stretch fix
	{-1.f,	 2.f,	-1.f}, {-1.f,	 1.f,	-1.f},
	{ 1.f,	 2.f,	-1.f}, { 1.f,	 1.f,	-1.f},
	{-1.f,	 2.f,	 1.f}, {-1.f,	 1.f,	 1.f},
	{ 1.f,	 2.f,	 1.f}, { 1.f,	 1.f,	 1.f},
	*/
	{-1.f, 1.f, -1.f}, {-1.f, 1.f, -1.f},
	{1.f, 1.f, -1.f}, {1.f, 1.f, -1.f},
	{-1.f, 1.f, 1.f}, {-1.f, 1.f, 1.f},
	{1.f, 1.f, 1.f}, {1.f, 1.f, 1.f},
	/* AVO: end */
	{-1.f, 0.f, -1.f}, {-1.f, -1.f, -1.f}, // half
	{1.f, 0.f, -1.f}, {1.f, -1.f, -1.f}, // half
	{1.f, 0.f, 1.f}, {1.f, -1.f, 1.f}, // half
	{-1.f, 0.f, 1.f}, {-1.f, -1.f, 1.f} // half
};
static u16 hbox_faces[20 * 3] =
{
	0, 2, 3,
	3, 1, 0,
	4, 5, 7,
	7, 6, 4,
	0, 1, 9,
	9, 8, 0,
	8, 9, 5,
	5, 4, 8,
	1, 3, 10,
	10, 9, 1,
	9, 10, 7,
	7, 5, 9,
	3, 2, 11,
	11, 10, 3,
	10, 11, 6,
	6, 7, 10,
	2, 0, 8,
	8, 11, 2,
	11, 8, 4,
	4, 6, 11
};

#pragma pack(push,1)
struct v_skybox
{
	Fvector3 p;
	u32 color;
	Fvector3 uv [2];

	void set(Fvector3& _p, u32 _c, Fvector3& _tc)
	{
		p = _p;
		color = _c;
		uv[0] = _tc;
		uv[1] = _tc;
	}
};

const u32 v_skybox_fvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX2 | D3DFVF_TEXCOORDSIZE3(0) | D3DFVF_TEXCOORDSIZE3(1);

struct v_clouds
{
	Fvector3 p;
	u32 color;
	u32 intensity;

	void set(Fvector3& _p, u32 _c, u32 _i)
	{
		p = _p;
		color = _c;
		intensity = _i;
	}
};

const u32 v_clouds_fvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_SPECULAR;
#pragma pack(pop)

void dxEnvDescriptorRender::Copy(IEnvDescriptorRender& _in)
{
	*this = *(dxEnvDescriptorRender*)&_in;
}

void dxEnvDescriptorMixerRender::Copy(IEnvDescriptorMixerRender& _in)
{
	dxEnvDescriptorMixerRender* other = (dxEnvDescriptorMixerRender*)&_in;

	clouds_r_textures.assign(other->clouds_r_textures.begin(), other->clouds_r_textures.end());
	clouds_r_textures.dwReference++;

	sky_r_textures.assign(other->sky_r_textures.begin(), other->sky_r_textures.end());
	sky_r_textures.dwReference++;

	sky_r_textures_env.assign(other->sky_r_textures_env.begin(), other->sky_r_textures_env.end());
	sky_r_textures_env.dwReference++;
}

void dxEnvironmentRender::Copy(IEnvironmentRender& _in)
{
	*this = *(dxEnvironmentRender*)&_in;
}

particles_systems::library_interface const& dxEnvironmentRender::particles_systems_library()
{
	return (RImplementation.PSLibrary);
}

void dxEnvDescriptorMixerRender::Destroy()
{
	sky_r_textures.clear();
	sky_r_textures_env.clear();
	clouds_r_textures.clear();
}

void dxEnvDescriptorMixerRender::Clear()
{
	std::pair<u32, ref_texture> zero = mk_pair(u32(0), ref_texture(0));
	sky_r_textures.clear();
	sky_r_textures.push_back(zero);
	sky_r_textures.push_back(zero);
	sky_r_textures.push_back(zero);

	sky_r_textures_env.clear();
	sky_r_textures_env.push_back(zero);
	sky_r_textures_env.push_back(zero);
	sky_r_textures_env.push_back(zero);

	clouds_r_textures.clear();
	clouds_r_textures.push_back(zero);
	clouds_r_textures.push_back(zero);
	clouds_r_textures.push_back(zero);
}

void dxEnvDescriptorMixerRender::lerp(IEnvDescriptorRender* inA, IEnvDescriptorRender* inB)
{
	dxEnvDescriptorRender* pA = (dxEnvDescriptorRender *)inA;
	dxEnvDescriptorRender* pB = (dxEnvDescriptorRender *)inB;

	sky_r_textures.clear();
	sky_r_textures.push_back(mk_pair(0, pA->sky_texture));
	sky_r_textures.push_back(mk_pair(1, pB->sky_texture));

	sky_r_textures_env.clear();

	sky_r_textures_env.push_back(mk_pair(0, pA->sky_texture_env));
	sky_r_textures_env.push_back(mk_pair(1, pB->sky_texture_env));

	clouds_r_textures.clear();
	clouds_r_textures.push_back(mk_pair(0, pA->clouds_texture));
	clouds_r_textures.push_back(mk_pair(1, pB->clouds_texture));
}

#ifdef USE_DX11
// Weather sky textures on demand (r4_sky_on_demand): GAMMA's weathers have ~1.2 GB of uncompressed sky textures for
// every weather and hour, and two keyframes are drawn at a time. They are created unloaded and load in the background:
// the next keyframe's ahead of time; a keyframe's that isn't loaded when it is drawn (a forced weather change) is
// drawn with the textures shown before until it is, so nothing waits. Ones not used for a while are unloaded.
int ps_r4_sky_on_demand = 1;
extern thread_local bool g_texture_create_on_demand;
extern xr_task_group textures_load_tasks;

namespace sky_on_demand
{
	static xr_vector<ref_texture> s_textures; // every weather texture, kept alive while the device is
	static xr_vector<CTexture*> s_loading; // background loads asked for and not finished
	static ref_texture s_shown[3][2]; // per list (sky, sky env, clouds) and keyframe: the last loaded texture drawn
	static u32 s_next_sweep = 0;
	const u32 kUnusedMs = 10000;

	static void add(ref_texture& T)
	{
		if (!T)
			return;
		for (ref_texture& t : s_textures)
		{
			if (&*t == &*T)
				return;
		}
		s_textures.push_back(T);
	}

	static void prefetch(ref_texture& T, u32 now)
	{
		if (!T)
			return;
		T->m_last_used = now;
		CTexture* raw = &*T;
		if (T->flags.bLoaded || T->flags.bLoading || std::find(s_loading.begin(), s_loading.end(), raw) != s_loading.end())
			return;
		s_loading.push_back(raw);
		textures_load_tasks.run([raw]() { raw->Load(); });
	}

	// The mixer's list for this frame: textures still loading are replaced by the ones drawn before
	template <typename List>
	static void resolve(List& list, int kind, u32 now)
	{
		for (size_t i = 0; i < list.size(); i++)
		{
			ref_texture& T = list[i].second;
			if (!T)
				continue;
			const int slot = i ? 1 : 0;
			T->m_last_used = now;
			if (T->flags.bLoaded && !T->flags.bLoading)
			{
				s_shown[kind][slot] = T;
				continue;
			}
			prefetch(T, now);
			ref_texture& shown = s_shown[kind][slot];
			if (shown && shown->flags.bLoaded && !shown->flags.bLoading)
			{
				shown->m_last_used = now;
				T = shown;
			}
			else
				T->Load(); // nothing drawn before (the level's first frame): load it now, waits for a background load
		}
	}

	static void sweep(u32 now)
	{
		s_loading.erase(std::remove_if(s_loading.begin(), s_loading.end(), [](CTexture* T) { return T->flags.bLoaded && !T->flags.bLoading; }), s_loading.end());
		if (now < s_next_sweep)
			return;
		s_next_sweep = now + 1000;
		for (ref_texture& T : s_textures)
		{
			if (T->m_on_demand && T->flags.bLoaded && !T->flags.bLoading && now - T->m_last_used > kUnusedMs &&
				std::find(s_loading.begin(), s_loading.end(), &*T) == s_loading.end())
				T->Unload();
		}
	}

	static void clear()
	{
		textures_load_tasks.wait();
		s_loading.clear();
		for (auto& kind : s_shown)
			for (ref_texture& T : kind)
				T = nullptr;
		s_textures.clear();
	}
}
#endif

void dxEnvDescriptorRender::OnDeviceCreate(CEnvDescriptor& owner)
{
#ifdef USE_DX11
	struct on_demand_scope
	{
		bool prev;
		on_demand_scope() : prev(g_texture_create_on_demand) { g_texture_create_on_demand = ps_r4_sky_on_demand != 0; }
		~on_demand_scope() { g_texture_create_on_demand = prev; }
	} on_demand;
#endif
	dxEnvDescriptorMixerRender& mixRen = *(dxEnvDescriptorMixerRender*)&*g_pGamePersistent
	                                                                     ->Environment().CurrentEnv->m_pDescriptorMixer;

	if (owner.sky_texture_name.size())
	{
		sky_texture.create(owner.sky_texture_name.c_str());
		if (g_pGamePersistent->Environment().m_paused)
		{
			mixRen.sky_r_textures.clear();
			mixRen.sky_r_textures.push_back(mk_pair(0, sky_texture));
			mixRen.sky_r_textures.push_back(mk_pair(1, sky_texture));
		}
	}

	if (owner.sky_texture_env_name.size())
	{
		sky_texture_env.create(owner.sky_texture_env_name.c_str());
		// The combine pass copies this cube with surface_get and never applies it
		sky_texture_env->bNoEvict = true;
		if (g_pGamePersistent->Environment().m_paused)
		{
			mixRen.sky_r_textures_env.clear();
			mixRen.sky_r_textures_env.push_back(mk_pair(0, sky_texture_env));
			mixRen.sky_r_textures_env.push_back(mk_pair(1, sky_texture_env));
		}
	}

	if (owner.clouds_texture_name.size())
	{
		clouds_texture.create(owner.clouds_texture_name.c_str());
		if (g_pGamePersistent->Environment().m_paused)
		{
			mixRen.clouds_r_textures.clear();
			mixRen.clouds_r_textures.push_back(mk_pair(0, clouds_texture));
			mixRen.clouds_r_textures.push_back(mk_pair(1, clouds_texture));
		}
	}

#ifdef USE_DX11
	sky_on_demand::add(sky_texture);
	sky_on_demand::add(sky_texture_env);
	sky_on_demand::add(clouds_texture);
#endif
}

#ifdef USE_DX11
void dxEnvDescriptorRender::residency(u32 now)
{
	sky_on_demand::prefetch(sky_texture, now);
	sky_on_demand::prefetch(sky_texture_env, now);
	sky_on_demand::prefetch(clouds_texture, now);
}
#endif

void dxEnvDescriptorRender::OnDeviceDestroy()
{
	sky_texture.destroy();
	sky_texture_env.destroy();
	clouds_texture.destroy();
}

dxEnvironmentRender::dxEnvironmentRender()
{
	tsky0 = DEV->_CreateTexture("$user$sky0");
	tsky1 = DEV->_CreateTexture("$user$sky1");
}

void dxEnvironmentRender::OnFrame(CEnvironment& env)
{
	PROF_EVENT("EnvironmentRender::OnFrame");
	dxEnvDescriptorMixerRender& mixRen = *(dxEnvDescriptorMixerRender*)&*env.CurrentEnv->m_pDescriptorMixer;

	if (::Render->get_generation() == IRender_interface::GENERATION_R2)
	{
		//. very very ugly hack
		if (HW.Caps.raster_major >= 3 && HW.Caps.geometry.bVTF)
		{
			// tonemapping in VS
			mixRen.sky_r_textures.push_back(mk_pair(u32(D3DVERTEXTEXTURESAMPLER0), tonemap)); //. hack
			mixRen.sky_r_textures_env.push_back(mk_pair(u32(D3DVERTEXTEXTURESAMPLER0), tonemap)); //. hack
			mixRen.clouds_r_textures.push_back(mk_pair(u32(D3DVERTEXTEXTURESAMPLER0), tonemap)); //. hack
		}
		else
		{
			// tonemapping in PS
			mixRen.sky_r_textures.push_back(mk_pair(2, tonemap)); //. hack
			mixRen.sky_r_textures_env.push_back(mk_pair(2, tonemap)); //. hack
			mixRen.clouds_r_textures.push_back(mk_pair(2, tonemap)); //. hack
		}
	}

#ifdef USE_DX11
	// Weather textures on demand: the two keyframes drawn now and the next one loading in the background, the
	// mixer drawing what is loaded, unused ones unloaded
	{
		const u32 now = Device.dwTimeContinual;
		for (int i = 0; i < 2; i++)
		{
			if (env.Current[i])
				((dxEnvDescriptorRender*)&*env.Current[i]->m_pDescriptor)->residency(now);
		}
		if (env.CurrentWeather && env.Current[1] && !env.CurrentWeather->empty())
		{
			auto& list = *env.CurrentWeather;
			for (size_t i = 0; i < list.size(); i++)
			{
				if (list[i] == env.Current[1])
				{
					CEnvDescriptor* next = list[(i + 1) % list.size()];
					((dxEnvDescriptorRender*)&*next->m_pDescriptor)->residency(now);
					break;
				}
			}
		}
		sky_on_demand::resolve(mixRen.sky_r_textures, 0, now);
		sky_on_demand::resolve(mixRen.sky_r_textures_env, 1, now);
		sky_on_demand::resolve(mixRen.clouds_r_textures, 2, now);
		sky_on_demand::sweep(now);
	}
#endif

	//. Setup skybox textures, somewhat ugly
	ID3DBaseTexture* e0 = mixRen.sky_r_textures[0].second->surface_get();
	ID3DBaseTexture* e1 = mixRen.sky_r_textures[1].second->surface_get();

	tsky0->surface_set(e0);
	_RELEASE(e0);
	tsky1->surface_set(e1);
	_RELEASE(e1);

	// ******************** Environment params (setting)
#if defined(USE_DX10) || defined(USE_DX11)
	//	TODO: DX10: Implement environment parameters setting for DX10 (if necessary)
#else	//	USE_DX10

#if		RENDER==R_R1
	Fvector3 fog_color = env.CurrentEnv->fog_color;
	fog_color.mul(ps_r1_fog_luminance);
#else	//	RENDER==R_R1
	Fvector3& fog_color = env.CurrentEnv->fog_color;
#endif	//	RENDER==R_R1

	CHK_DX(HW.pDevice->SetRenderState( D3DRS_FOGCOLOR, color_rgba_f(fog_color.x,fog_color.y,fog_color.z,0) ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_FOGSTART, *(u32 *)(&env.CurrentEnv->fog_near) ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_FOGEND, *(u32 *)(&env.CurrentEnv->fog_far) ));
#endif	//	USE_DX10
}

void dxEnvironmentRender::OnLoad()
{
	tonemap = DEV->_CreateTexture("$user$tonemap"); //. hack
}

void dxEnvironmentRender::OnUnload()
{
	tonemap = 0;
}

void dxEnvironmentRender::RenderSky(CEnvironment& env, bool OnlyMV)
{
	PROF_EVENT("EnvironmentRender::RenderSky");
	// clouds_sh.create		("clouds","null");
	//. this is the bug-fix for the case when the sky is broken
	//. for some unknown reason the geoms happen to be invalid sometimes
	//. if vTune show this in profile, please add simple cache (move-to-forward last found) 
	//. to the following functions:
	//.		CResourceManager::_CreateDecl
	//.		CResourceManager::CreateGeom
	if (env.bNeed_re_create_env)
	{
		sh_2sky.create(&m_b_skybox, "skybox_2t");
		sh_2geom.create(v_skybox_fvf, RCache.Vertex.Buffer(), RCache.Index.Buffer());
		clouds_sh.create("clouds", "null");
		clouds_geom.create(v_clouds_fvf, RCache.Vertex.Buffer(), RCache.Index.Buffer());
		env.bNeed_re_create_env = FALSE;
	}
	::Render->rmFar();

	dxEnvDescriptorMixerRender& mixRen = *(dxEnvDescriptorMixerRender*)&*env.CurrentEnv->m_pDescriptorMixer;

	// draw sky box
	Fmatrix mSky;
	mSky.rotateY(env.CurrentEnv->sky_rotation);
	mSky.translate_over(Device.vCameraPosition);

	u32 i_offset, v_offset;
	u32 C = color_rgba(iFloor(env.CurrentEnv->sky_color.x * 255.f), iFloor(env.CurrentEnv->sky_color.y * 255.f),
	                   iFloor(env.CurrentEnv->sky_color.z * 255.f), iFloor(env.CurrentEnv->weight * 255.f));

	// Fill index buffer
	u16* pib = RCache.Index.Lock(20 * 3, i_offset);
	CopyMemory(pib, hbox_faces, 20*3*2);
	RCache.Index.Unlock(20 * 3);

	// Fill vertex buffer
	v_skybox* pv = (v_skybox*)RCache.Vertex.Lock(12, sh_2geom.stride(), v_offset);
	for (u32 v = 0; v < 12; v++) pv[v].set(hbox_verts[v * 2], C, hbox_verts[v * 2 + 1]);
	RCache.Vertex.Unlock(12, sh_2geom.stride());

	// Apply skybox matrix
	RCache.set_xform_world(mSky);

	if (OnlyMV)
	{
		RCache.set_xform_world_prev(mSky_prev);
		mSky_prev = mSky;

		RCache.set_Geometry(sh_2geom);
		RCache.set_Shader(sh_2sky);
		RCache.set_Element(sh_2sky->E[1]);
		RCache.Render(D3DPT_TRIANGLELIST, v_offset, 0, 12, i_offset, 20);

		RCache.set_xform_world_prev(Fidentity);
	}
	else
	{
		RCache.set_Geometry(sh_2geom);
		RCache.set_Shader(sh_2sky);
		RCache.set_Element(sh_2sky->E[0]);
		RCache.set_Textures(&mixRen.sky_r_textures);
		RCache.Render(D3DPT_TRIANGLELIST, v_offset, 0, 12, i_offset, 20);
	}

	// Sun
	::Render->rmNormal();

	if (!OnlyMV)
	{
#if	RENDER!=R_R1
		//
		// This hack is done to make sure that the state is set for sure:
		// The state may be not set by RCache if the state is changed using API SetRenderState() function before 
		// and the RCache flag will remain unchanged to it's old value. 
		// 
		RCache.set_Z(FALSE);
		RCache.set_Z(TRUE);
		env.eff_LensFlare->Render(TRUE, FALSE, FALSE);
		RCache.set_Z(FALSE);
#else
		env.eff_LensFlare->Render(TRUE, FALSE, FALSE);
#endif
	}
}

void dxEnvironmentRender::RenderClouds(CEnvironment& env)
{
	PROF_EVENT("EnvironmentRender::RenderClouds");
	::Render->rmFar();

	Fmatrix mXFORM, mScale;
	mScale.scale(10, 0.4f, 10);
	mXFORM.rotateY(env.CurrentEnv->sky_rotation);
	mXFORM.mulB_43(mScale);
	mXFORM.translate_over(Device.vCameraPosition);

	Fvector wd0, wd1;
	Fvector4 wind_dir;
	wd0.setHP(PI_DIV_4, 0);
	wd1.setHP(PI_DIV_4 + PI_DIV_8, 0);
	wind_dir.set(wd0.x, wd0.z, wd1.x, wd1.z).mul(0.5f).add(0.5f).mul(255.f);
	u32 i_offset, v_offset;
	u32 C0 = color_rgba(iFloor(wind_dir.x), iFloor(wind_dir.y), iFloor(wind_dir.w), iFloor(wind_dir.z));
	u32 C1 = color_rgba(iFloor(env.CurrentEnv->clouds_color.x * 255.f), iFloor(env.CurrentEnv->clouds_color.y * 255.f),
	                    iFloor(env.CurrentEnv->clouds_color.z * 255.f), iFloor(env.CurrentEnv->clouds_color.w * 255.f));

	// Fill index buffer
	u16* pib = RCache.Index.Lock(env.CloudsIndices.size(), i_offset);
	CopyMemory(pib, &env.CloudsIndices.front(), env.CloudsIndices.size()*sizeof(u16));
	RCache.Index.Unlock(env.CloudsIndices.size());

	// Fill vertex buffer
	v_clouds* pv = (v_clouds*)RCache.Vertex.Lock(env.CloudsVerts.size(), clouds_geom.stride(), v_offset);
	for (FvectorIt it = env.CloudsVerts.begin(); it != env.CloudsVerts.end(); it++, pv++)
		pv->set(*it, C0, C1);
	RCache.Vertex.Unlock(env.CloudsVerts.size(), clouds_geom.stride());

	// Render
	RCache.set_xform_world(mXFORM);
	RCache.set_Geometry(clouds_geom);
	RCache.set_Shader(clouds_sh);
	dxEnvDescriptorMixerRender& mixRen = *(dxEnvDescriptorMixerRender*)&*env.CurrentEnv->m_pDescriptorMixer;
	RCache.set_Textures(&mixRen.clouds_r_textures);
	RCache.Render(D3DPT_TRIANGLELIST, v_offset, 0, env.CloudsVerts.size(), i_offset, env.CloudsIndices.size() / 3);

	::Render->rmNormal();
}

void dxEnvironmentRender::OnDeviceCreate()
{
	sh_2sky.create(&m_b_skybox, "skybox_2t");
	sh_2geom.create(v_skybox_fvf, RCache.Vertex.Buffer(), RCache.Index.Buffer());
	clouds_sh.create("clouds", "null");
	clouds_geom.create(v_clouds_fvf, RCache.Vertex.Buffer(), RCache.Index.Buffer());
}

void dxEnvironmentRender::OnDeviceDestroy()
{
#ifdef USE_DX11
	sky_on_demand::clear();
#endif
	tsky0->surface_set(NULL);
	tsky1->surface_set(NULL);

	sh_2sky.destroy();
	sh_2geom.destroy();
	clouds_sh.destroy();
	clouds_geom.destroy();
}
