#include "stdafx.h"

#include "../../xrEngine/render.h"
#include "../../xrEngine/irenderable.h"
#include "../../xrEngine/igame_persistent.h"
#include "../../xrEngine/environment.h"
#include "../../xrEngine/CustomHUD.h"

#include "FBasicVisual.h"
#include "CHudInitializer.h"

#include "fhierrarhyvisual.h"
#include "SkeletonCustom.h"
#include "../../xrEngine/fmesh.h"
#include "flod.h"

#include "../../xrEngine/xr_object.h"

using namespace R_dsgraph;

extern float r_ssaDISCARD;
extern float r_ssaDONTSORT;
extern float r_ssaHZBvsTEX;
extern float r_ssaGLOD_start, r_ssaGLOD_end;

#ifdef USE_DX11
namespace hiz { bool test_dynamic(const Fsphere& sphere); }
#endif

ICF bool R4FullDetailRejectSpatial(ISpatial* spatial, bool normalPhase)
{
#if RENDER == R_R4
	if (ps_r4_full_detail_distance_scale >= 1.f || !normalPhase || !spatial || !g_pGamePersistent ||
		!g_pGamePersistent->Environment().CurrentEnv)
		return false;

	const float farPlane = g_pGamePersistent->Environment().CurrentEnv->far_plane;
	const float fullDetailDistance = std::max(100.f, farPlane * ps_r4_full_detail_distance_scale);
	if (Device.vCameraPosition.distance_to_sqr(spatial->spatial.sphere.P) <=
		_sqr(fullDetailDistance + spatial->spatial.sphere.R))
		return false;

	if (spatial->spatial.type & STYPE_LIGHTSOURCE)
	{
		light* L = (light*)spatial->dcast_Light();
		if (L && L->flags.bHudMode)
			return false;
	}

	return true;
#else
	return false;
#endif
}

ICF float calcLOD(float ssa/*fDistSq*/, float R)
{
	return _sqrt(clampr((ssa - r_ssaGLOD_end) / (r_ssaGLOD_start - r_ssaGLOD_end), 0.f, 1.f));
}

#ifdef USE_DX11
// Previous-frame world matrix of dynamic visuals, by their xform pointer (from brodrigz/xray-monolith 353e4bf48). Only the
// sky set the previous world matrix, so rigid dynamic meshes (props, items, doors) built m_wvp_prev with an identity world
// and wrote motion vectors as if they came from the world's origin: smearing and ghosting under DLSS/FSR and SSS TAA.
// Skinned meshes still set their own m_wvp_prev (CSkeletonX::_Render)
namespace
{
struct PrevWorldXform
{
	Fmatrix prev;
	Fmatrix last;
	u32 frame;
};
xr_unordered_map<const Fmatrix*, PrevWorldXform> g_prev_world_xforms;

void set_dynamic_world_prev(const Fmatrix* M)
{
	if (!RImplementation.o.ssfx_motionvectors)
		return;
	const size_t count = g_prev_world_xforms.size();
	PrevWorldXform& X = g_prev_world_xforms[M];
	if (g_prev_world_xforms.size() != count)
	{
		X.prev = *M;
		X.frame = Device.dwFrame;
	}
	else if (X.frame != Device.dwFrame)
	{
		// Not drawn last frame: no usable history, so no object motion
		X.prev = (X.frame + 1 == Device.dwFrame) ? X.last : *M;
		X.frame = Device.dwFrame;
	}
	X.last = *M;
	RCache.set_xform_world_prev(X.prev);
}

void prune_dynamic_world_prev()
{
	static u32 last_prune = 0;
	if (Device.dwFrame - last_prune < 600)
		return;
	last_prune = Device.dwFrame;
	for (auto it = g_prev_world_xforms.begin(); it != g_prev_world_xforms.end();)
	{
		if (Device.dwFrame - it->second.frame > 2)
			it = g_prev_world_xforms.erase(it);
		else
			++it;
	}
}
}
#endif

#ifdef USE_DX11
#include "FTreeVisual.h"
#include "dxRenderDeviceRender.h"
#ifdef USE_DX11
#include "../xrRenderPC_R4/r4_gpu_profiler.h"

extern int ps_r4_gpu_passes_split;

// r4_gpu_passes_split: the main view's static geometry and the shadow maps timed per pixel shader (stat_gpu_passes), by
// the shader's name
static LPCWSTR gpu_scope_name(ID3DPixelShader* ps)
{
	// The profiler keeps the names until the frame's results arrive: never freed
	static xr_unordered_map<ID3DPixelShader*, LPCWSTR> names;
	auto it = names.find(ps);
	if (it != names.end())
		return it->second;
	LPCSTR name = "ps_unknown";
	for (const auto& it_ps : DEV->pixel_shaders())
		if (it_ps.second->ps == ps)
		{
			name = it_ps.first;
			break;
		}
	const u32 n = _min(u32(xr_strlen(name)), 60u);
	wchar_t* wide = xr_alloc<wchar_t>(n + 1);
	for (size_t i = 0; i < n; ++i)
		wide[i] = wchar_t(u8(name[i]));
	wide[n] = 0;
	names.emplace(ps, wide);
	return wide;
}
#endif

extern int ps_r4_tree_instancing;

// Tree instancing (r4_tree_instancing): in the main view's G-buffer and the shadow maps, the trees of a run of packets
// with the same state and geometry are drawn with one DrawIndexedInstanced. Their pass gets a variant whose vertex
// shader is the tree shader's own source with its per-tree constants read per instance (<name>_xrinst, made in memory
// when compiled: dx10ResourceManager_Resources.cpp); the trees' constants go to xr_tree_inst. A shader that can't be
// changed that way keeps drawing its trees one by one
namespace tree_instancing
{
	constexpr u32 instances_max = 32; // xr_tree_inst's size in the patched shader

	struct variant
	{
		const SVS* orig_vs = nullptr;
		const R_constant_table* orig_constants = nullptr;
		ref_vs vs;
		ref_ctable constants;
		bool valid = false;
	};
	static xr_unordered_map<u64, variant> s_variants; // by the original pass
	static shared_str s_instances_name;

	static variant* get(SPass& pass)
	{
		variant& v = s_variants[u64(size_t(&pass))];
		if (v.orig_vs == &*pass.vs && v.orig_constants == &*pass.constants)
			return v.valid ? &v : nullptr;

		// A new pass, or another one at the same address
		v = variant();
		v.orig_vs = &*pass.vs;
		v.orig_constants = &*pass.constants;
		if (!pass.vs || !pass.constants || !pass.vs->vs)
			return nullptr;
		LPCSTR orig = pass.vs->cName.c_str();
		if (strncmp(orig, "deffer_tree", 11) && strncmp(orig, "shadow_direct_tree", 18))
			return nullptr;
		string256 name;
		xr_sprintf(name, "%s_xrinst", orig);
		v.vs = DEV->_CreateVS(name);
		if (!v.vs || !v.vs->vs)
			return nullptr;

		// The pass's constants with this vertex shader, bound as in the original pass
		R_constant_table table;
		table.merge(&pass.ps->constants);
		table.merge(&v.vs->constants);
		table.merge(&pass.gs->constants);
		table.merge(&pass.hs->constants);
		table.merge(&pass.ds->constants);
		for (ref_constant& c : table.table)
		{
			if (R_constant* o = pass.constants->get(c->name))
				c->handler = o->handler;
		}
		if (!s_instances_name.size())
			s_instances_name = "xr_tree_inst";
		if (!table.get(s_instances_name))
			return nullptr;
		v.constants = DEV->_CreateConstantTable(table);
		v.valid = true;
		static xr_vector<shared_str> logged;
		if (std::find(logged.begin(), logged.end(), v.vs->cName) == logged.end())
		{
			logged.push_back(v.vs->cName);
			Msg("* Tree instancing: %s", name);
		}
		return &v;
	}

	static FTreeVisual* tree_of(const RenderPacket& packet)
	{
		dxRender_Visual* V = packet.item.pVisual;
		if (V->Type != MT_TREE_ST && V->Type != MT_TREE_PM)
			return nullptr;
		return static_cast<FTreeVisual*>(V);
	}

	// The packet's pass: the one of the visual's shader with its shaders
	static SPass* pass_of(const RenderPacket& packet, u32 iPass)
	{
		Shader* sh = &*packet.item.pVisual->shader;
		if (!sh)
			return nullptr;
		for (u32 e = 0; e < 6; ++e)
		{
			ShaderElement* E = sh->E[e]._get();
			if (!E || iPass >= E->passes.size())
				continue;
			SPass* pass = &*E->passes[iPass];
			if (&*pass->vs == packet.pVS && pass->ps->ps == packet.pPS && &*pass->constants == packet.pCS)
				return pass;
		}
		return nullptr;
	}
}

void tree_instancing_reset()
{
	tree_instancing::s_variants.clear();
}
#endif

namespace
{
class CCustomFOVInitializer
{
	bool enabled;
	bool near_depth;
	Fmatrix saved_project;
	Fmatrix saved_full;

public:
	explicit CCustomFOVInitializer(float fov, bool adjust_depth = false)
		: enabled(fov != 0.f), near_depth(adjust_depth)
	{
		if (!enabled) return;
		saved_project = RCache.get_xform_project();
		saved_full = Device.mFullTransform;
		Fmatrix project;
		project.build_projection(deg2rad(fov), Device.fASPECT, R_VIEWPORT_NEAR,
			g_pGamePersistent->Environment().CurrentEnv->far_plane);
		if (near_depth) RImplementation.rmNear();
		Device.mFullTransform.mul(project, Device.mView);
		RCache.set_xform_project(project);
	}

	~CCustomFOVInitializer()
	{
		if (!enabled) return;
		if (near_depth) RImplementation.rmNormal();
		Device.mFullTransform = saved_full;
		RCache.set_xform_project(saved_project);
	}
};
}

template<typename T, bool Reverse>
void CDSGraphManager::r_dsgraph_render_graph_sorted(R_dsgraph::mapDSGraphItems<T, Reverse>& graph, bool _clear)
{
    if (graph.empty())
        return;

    std::sort(graph.begin(), graph.end());

	for (auto& item : graph)
	{
		dxRender_Visual* V = item.pVisual;
		VERIFY(V && V->shader._get());
		RCache.set_Element(item.pSE);
		CCustomFOVInitializer projection(item.fov);
		RCache.set_xform_world(*item.pMatrix);
#ifdef USE_DX11
		set_dynamic_world_prev(item.pMatrix);
#endif
		RImplementation.apply_object(item.pObject);
		RImplementation.apply_lmaterial();
		//if (item.b_hud_mode)
		//{
		//	//new feature
		//}
		V->Render(calcLOD(item.ssa, V->vis.sphere.R));
	}

	if (_clear)
		graph.clear();

	RCache.set_xform_world(Fidentity);
#ifdef USE_DX11
	RCache.set_xform_world_prev(Fidentity);
#endif
}

void CDSGraphManager::r_dsgraph_render_graph(RenderQueueArray& queues, u32 _priority, bool _clear, bool static_geometry)
{
	RCache.set_xform_world(Fidentity);
#ifdef USE_DX11
	RCache.set_xform_world_prev(Fidentity);
	if (!static_geometry)
		prune_dynamic_world_prev();
#endif

	for (u32 iPass = 0; iPass < SHADER_PASSES_MAX; ++iPass)
	{
		auto& queue = queues[_priority][iPass];
		if (queue.empty())
			continue;

		// 1. Sort by generated sort key to replicate previous fixed map behaviour
		if (queue.size() < 4096)
            std::sort(queue.begin(), queue.end());
		else
            xr_parallel_sort(queue.begin(), queue.end());

#ifdef USE_DX11
		// Trees of the same geometry next to each other in each run of the same state (instKey, set when captured), the
		// ones that share a draw first and the single ones after them (instKey 0), so the run switches shaders once
		const bool instancing = ps_r4_tree_instancing && static_geometry &&
			(RImplementation.phase == CRender::PHASE_NORMAL || RImplementation.phase == CRender::PHASE_SMAP);
		if (instancing)
		{
			for (size_t begin = 0, end; begin < queue.size(); begin = end)
			{
				end = begin + 1;
				while (end < queue.size() && queue[end].sortKey == queue[begin].sortKey)
					end++;
				if (!queue[begin].instKey)
					continue;
				if (end - begin < 2)
				{
					queue[begin].instKey = 0;
					continue;
				}
				PROF_EVENT("tree instancing: order");
				std::sort(queue.begin() + begin, queue.begin() + end,
					[](const RenderPacket& a, const RenderPacket& b) { return a.instKey < b.instKey; });
				for (size_t i = begin, run_end; i < end; i = run_end)
				{
					run_end = i + 1;
					while (run_end < end && queue[run_end].instKey == queue[i].instKey)
						run_end++;
					if (run_end - i == 1)
						queue[i].instKey = 0;
				}
				// 0 - 1 wraps to the largest: the single ones last
				std::sort(queue.begin() + begin, queue.begin() + end,
					[](const RenderPacket& a, const RenderPacket& b) { return a.instKey - 1 < b.instKey - 1; });
			}
		}
#endif

		// 2. Render
		vs_type pVS = nullptr;

#if defined(USE_DX10) || defined(USE_DX11)
		gs_type pGS = nullptr;
#endif

		ps_type pPS = nullptr;

#ifdef USE_DX11
		hs_type pHS = nullptr;
		ds_type pDS = nullptr;
#endif
		
		R_constant_table* pCS = nullptr;
		ID3DState* pState = nullptr;
#ifdef USE_DX11
		vs_type inst_for_vs = nullptr;
		R_constant_table* inst_for_cs = nullptr;
		tree_instancing::variant* inst_cached = nullptr;
#endif
		STextureList* pTextures = nullptr;
#ifdef USE_DX11
		const bool split = ps_r4_gpu_passes_split && (RImplementation.phase == CRender::PHASE_NORMAL ? static_geometry : RImplementation.phase == CRender::PHASE_SMAP);
		ps_type split_ps = nullptr;
#endif

        u64 high = 0;

        for (size_t packet_index = 0; packet_index < queue.size(); ++packet_index)
        {
            auto& packet = queue[packet_index];
            auto& currentKey = packet.sortKey;
            if (currentKey.high != high)
            {
                high = currentKey.high;

                if (packet.pState != pState)
                {
                    pState = packet.pState;
                    RCache.set_States(pState);
                }

#if defined(USE_DX10) || defined(USE_DX11)
                if (packet.pGS != pGS)
                {
                    pGS = packet.pGS;
                    RCache.set_GS(pGS);
                }
#endif

#ifdef USE_DX11
                if (packet.pHS != pHS)
                {
                    pHS = packet.pHS;
                    RCache.set_HS(pHS);
                }
                if (packet.pDS != pDS)
                {
                    pDS = packet.pDS;
                    RCache.set_DS(pDS);
                }
#endif
            }

#ifdef USE_DX11
			// Tree instancing: a run of two or more packets with the same state and geometry, drawn with the pass's
			// instanced variant (a single tree is drawn as before: its constants cost less than an instance's)
			tree_instancing::variant* inst = nullptr;
			size_t run_end = packet_index + 1;
			if (instancing && packet.instKey)
			{
				while (run_end < queue.size() && queue[run_end].instKey == packet.instKey && queue[run_end].sortKey == packet.sortKey)
					run_end++;
				// One pass for a state's trees: looked up once
				if (packet.pVS != inst_for_vs || packet.pCS != inst_for_cs)
				{
					inst_for_vs = packet.pVS;
					inst_for_cs = packet.pCS;
					SPass* pass = tree_instancing::pass_of(packet, iPass);
					inst_cached = pass ? tree_instancing::get(*pass) : nullptr;
				}
				inst = inst_cached;
			}
			vs_type packet_vs = inst ? &*inst->vs : packet.pVS;
			R_constant_table* packet_cs = inst ? &*inst->constants : packet.pCS;
#else
			vs_type packet_vs = packet.pVS;
			R_constant_table* packet_cs = packet.pCS;
#endif

            // Compare low key stuff regardless, too high collision probability
            if (packet_vs != pVS)
            {
                pVS = packet_vs;
                RCache.set_VS(pVS);
            }

#ifdef USE_DX11
			if (split && packet.pPS != split_ps)
			{
				if (split_ps)
					gpu_profiler::event_end();
				split_ps = packet.pPS;
				gpu_profiler::event_begin(gpu_scope_name(split_ps));
			}
#endif

            if (packet.pPS != pPS)
            {
                pPS = packet.pPS;
                RCache.set_PS(pPS);
            }

            if (packet_cs != pCS)
            {
                pCS = packet_cs;
                RCache.set_Constants(pCS);
            }

            if (packet.pTextures != pTextures)
            {
                pTextures = packet.pTextures;
                RCache.set_Textures(pTextures);
                RImplementation.apply_lmaterial();
            }

			auto& item = packet.item;
			// Opaque custom-FOV geometry keeps all of its original material passes.
			CCustomFOVInitializer projection(item.fov, true);
			if (!static_geometry)
			{
				RCache.set_xform_world(*item.pMatrix);
#ifdef USE_DX11
				set_dynamic_world_prev(item.pMatrix);
#endif
				RImplementation.apply_object(item.pObject);
				RImplementation.apply_lmaterial();
			}

#ifdef USE_DX11
			if (inst)
			{
				PROF_EVENT("tree instancing: draw");
				const u32 count = u32(run_end - packet_index);
				FTreeVisual* first = static_cast<FTreeVisual*>(item.pVisual);
				const float run_LOD = calcLOD(item.ssa, item.pVisual->vis.sphere.R);
				RCache.LOD.set_LOD(run_LOD);
				for (u32 done = 0; done < count; done += tree_instancing::instances_max)
				{
					const u32 n = _min(count - done, tree_instancing::instances_max);
					void* data = nullptr;
					RCache.get_ConstantDirect(tree_instancing::s_instances_name, n * FTreeVisual::instance_rows * sizeof(Fvector4),
						&data, nullptr, nullptr);
					if (!data)
						break;
					Fvector4* rows = (Fvector4*)data;
					for (u32 i = 0; i < n; i++)
						static_cast<FTreeVisual*>(queue[packet_index + done + i].item.pVisual)->write_instance(
							rows + i * FTreeVisual::instance_rows);
					first->render_instances(run_LOD, n);
				}
				packet_index = run_end - 1;
				continue;
			}
#endif
			float LOD = calcLOD(item.ssa, item.pVisual->vis.sphere.R);
#ifdef USE_DX11
			RCache.LOD.set_LOD(LOD);
#endif
			item.pVisual->Render(LOD);
		}
#ifdef USE_DX11
		if (split_ps)
			gpu_profiler::event_end();
#endif

		if (_clear)
			queue.clear();
	}
#ifdef USE_DX11
	if (!static_geometry)
		RCache.set_xform_world_prev(Fidentity);
#endif
}

//////////////////////////////////////////////////////////////////////////
// HUD render
void CDSGraphManager::r_dsgraph_render_hud()
{
	PROF_EVENT("r_dsgraph_render_hud");
	CHudInitializer initializer(true);

	// Rendering
	RImplementation.rmNear();
	r_dsgraph_render_graph_sorted(RGraph.mapHUD);
	r_dsgraph_render_graph_sorted(RGraph.mapCustomFOV);

	if (!RGraph.mapCamAttached.empty())
	{
		initializer.SetCamMode();
		r_dsgraph_render_graph_sorted(RGraph.mapCamAttached);
	}
	RImplementation.rmNormal();

#if defined(USE_DX11) //  Redotix99: for 3D Shader Based Scopes 		
	if (scope_3D_fake_enabled)
	{
		RCache.set_RT(RImplementation.Target->rt_ssfx_temp->pRT, 3); // Render scope_3D to any buffer

		initializer.SetHudMode();
		r_dsgraph_render_graph_sorted(RGraph.mapScopeHUD);
		initializer.SetCamMode();
		r_dsgraph_render_graph_sorted(RGraph.mapScopeCamAttached);
		initializer.SetHudMode();

		if (!RImplementation.o.ssfx_motionvectors)
			RCache.set_RT(NULL, 3);
		else
			RCache.set_RT(RImplementation.Target->rt_ssfx_motion_vectors->pRT, 3);
	}
	RGraph.mapScopeHUD.clear();
	RGraph.mapScopeCamAttached.clear();
#endif
}

void CDSGraphManager::r_dsgraph_render_hud_ui()
{
	PROF_EVENT("r_dsgraph_render_hud_ui");
	CHudInitializer initializer(true);

	RImplementation.rmNear();
	g_hud->RenderActiveItemUI();
	RImplementation.rmNormal();
}

void CDSGraphManager::r_dsgraph_render_cam_ui()
{
	PROF_EVENT("r_dsgraph_render_cam_ui");

	// Change projection
	CHudInitializer initializer(2);
	
	// Rendering
	RImplementation.rmNear();
	g_hud->RenderCamAttachedUI();
	RImplementation.rmNormal();
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void CDSGraphManager::r_dsgraph_render_sorted(bool render_hud)
{
	r_dsgraph_render_ui();
	{
		PROF_EVENT("r_dsgraph_render_sorted");
		// Rendering
		r_dsgraph_render_graph_sorted(RGraph.mapStaticSorted.Sorted, true);
		r_dsgraph_render_graph_sorted(RGraph.mapDynamicSorted.Sorted, true);
		r_dsgraph_render_graph_sorted(RGraph.mapCustomFOVSorted.Sorted, true);
	}

	if (render_hud)
		r_dsgraph_render_sorted_hud();

}

void CDSGraphManager::r_dsgraph_capture_hud()
{
	if (g_hud)
	{
		g_hud->Render_Last(dcast_IPortalTraverser());
		set_Object();
	}
}

#if defined(USE_DX11)
//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void CDSGraphManager::r_dsgraph_render_ScopeSorted()  //  Redotix99: for 3D Shader Based Scopes 	
{
	// Change projection
	CHudInitializer initializer(true);

	// Rendering
	RImplementation.rmNear();
	r_dsgraph_render_graph_sorted(RGraph.mapScopeHUDSorted, true);
	initializer.SetCamMode();
	r_dsgraph_render_graph_sorted(RGraph.mapScopeCamAttachedSorted, true);
	RImplementation.rmNormal();
}

// The PDA screen held back for after the upscaler (upscale_pda_late), as in r_dsgraph_render_sorted_hud
void CDSGraphManager::r_dsgraph_render_PDALate()
{
	CHudInitializer initializer(true);
	RImplementation.rmNear();
	r_dsgraph_render_graph_sorted(RGraph.mapPDALate, true);
	RImplementation.rmNormal();
}

// The WearableDevices screen glass held back with the HUD UI it covers (mapHUDGlassLate), as in r_dsgraph_render_sorted_hud
void CDSGraphManager::r_dsgraph_render_HUDGlassLate()
{
	if (RGraph.mapHUDGlassLate.empty())
		return;
	CHudInitializer initializer(true);
	RImplementation.rmNear();
	r_dsgraph_render_graph_sorted(RGraph.mapHUDGlassLate, true);
	RImplementation.rmNormal();
}
#endif

void CDSGraphManager::r_dsgraph_render_sorted_hud()
{
	PROF_EVENT("r_dsgraph_render_sorted_hud");
#if	RENDER==R_R4
	HW.pContext->CopyResource(RImplementation.Target->rt_Accumulator->pSurface, RImplementation.Target->rt_Generic_0->pSurface);
#endif
	CHudInitializer initializer(true);

	RImplementation.rmNear();
	r_dsgraph_render_graph_sorted(RGraph.mapHUDSorted.Sorted, true);
	initializer.SetCamMode();
	r_dsgraph_render_graph_sorted(RGraph.mapCamAttachedSorted.Sorted, true);
	RImplementation.rmNormal();
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void CDSGraphManager::r_dsgraph_render_emissive(bool clear, bool renderHUD)
{
	PROF_EVENT("r_dsgraph_render_emissive");
#if	RENDER!=R_R1
	r_dsgraph_render_graph_sorted(RGraph.mapStaticSorted.Emissive, clear);
	r_dsgraph_render_graph_sorted(RGraph.mapDynamicSorted.Emissive, clear);
	r_dsgraph_render_graph_sorted(RGraph.mapCustomFOVSorted.Emissive, clear);
	
	//	HACK: Calculate this only once
	CHudInitializer initializer(true);

	RImplementation.rmNear();
	r_dsgraph_render_graph_sorted(RGraph.mapHUDSorted.Emissive, clear);
	
	if (renderHUD)
		r_dsgraph_render_graph_sorted(RGraph.mapHUDSorted.Sorted, false);

	RImplementation.rmNormal();
#endif
}

void CDSGraphManager::r_dsgraph_render_water_ssr()
{
#ifdef USE_DX11
	PROF_EVENT("r_dsgraph_render_water_ssr");
	std::sort(RGraph.mapWater.begin(), RGraph.mapWater.end());
	for (auto& N : RGraph.mapWater)
	{
		dxRender_Visual* V = N.pVisual;
		VERIFY(V);

		RCache.set_Shader(RImplementation.Target->s_ssfx_water_ssr);

		RCache.set_xform_world(*N.pMatrix);
		RImplementation.apply_object(N.pObject);
		RImplementation.apply_lmaterial();

		RCache.set_c("cam_pos", RImplementation.Target->Position_previous.x, RImplementation.Target->Position_previous.y, RImplementation.Target->Position_previous.z, 0.0f);

		// Previous matrix data
		RCache.set_c("m_current", RImplementation.Target->Matrix_current);
		RCache.set_c("m_previous", RImplementation.Target->Matrix_previous);

		V->Render(calcLOD(N.ssa, V->vis.sphere.R));
	}
#endif
}

void CDSGraphManager::r_dsgraph_render_water()
{
	PROF_EVENT("r_dsgraph_render_water_ssr");
    std::sort(RGraph.mapWater.begin(), RGraph.mapWater.end());
    for (auto& N : RGraph.mapWater)
	{
		dxRender_Visual* V = N.pVisual;
		VERIFY(V);

#ifdef USE_DX11
		if (RImplementation.o.ssfx_water)
		{
			RCache.set_Shader(RImplementation.Target->s_ssfx_water);
		}
#endif

		RCache.set_xform_world(*N.pMatrix);
		RImplementation.apply_object(N.pObject);
		RImplementation.apply_lmaterial();

		// Wind settings
		float WindDir = g_pGamePersistent->Environment().CurrentEnv->wind_direction;
		float WindVel = g_pGamePersistent->Environment().CurrentEnv->wind_velocity;
		RCache.set_c("wind_setup", WindDir, WindVel, 0, 0);

		V->Render(calcLOD(N.ssa, V->vis.sphere.R));
	}
	RGraph.mapWater.clear();
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void CDSGraphManager::r_dsgraph_render_wmarks()
{
	PROF_EVENT("r_dsgraph_render_wmarks");
#if	RENDER!=R_R1
	// Rendering
	r_dsgraph_render_graph_sorted(RGraph.mapStaticSorted.Wmark);
	r_dsgraph_render_graph_sorted(RGraph.mapDynamicSorted.Wmark);
	//	HACK: Calculate this only once
	CHudInitializer initalizer(true);

	RImplementation.rmNear();
	r_dsgraph_render_graph_sorted(RGraph.mapHUDSorted.Wmark);
	RImplementation.rmNormal();
#endif
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void CDSGraphManager::r_dsgraph_render_distort()
{
	PROF_EVENT("r_dsgraph_render_distort");
	// Rendering
	r_dsgraph_render_graph_sorted(RGraph.mapStaticSorted.Distort, true);
	r_dsgraph_render_graph_sorted(RGraph.mapDynamicSorted.Distort, true);
	r_dsgraph_render_graph_sorted(RGraph.mapCustomFOVSorted.Distort, true);
	//	HACK: Calculate this only once
	CHudInitializer initalizer(true);

	RImplementation.rmNear();
	r_dsgraph_render_graph_sorted(RGraph.mapHUDSorted.Distort);
	RImplementation.rmNormal();
}

#include"LightTrack.h"
void CDSGraphManager::r_dsgraph_capture_static()
{
	PROF_EVENT("r_dsgraph_capture_static")
	const bool dbg_enabled = PortalTraverseDbg_Enabled();
	PortalTraverseDebugStats* dbg = dbg_enabled ? &PortalTraverseDbg_Get() : nullptr;
	const bool opt_bucket = PortalTraverseDbg_IsOptions(i_options);

	if (i_start)
	{
		// Traverse sector/portal structure
		if (psDeviceFlags.test(rsDrawStatic))
		{
			if (dbg)
			{
				const u32 sector_nodes = u32(m_sector_frustums.size());
				dbg->static_sector_nodes += sector_nodes;
				if (opt_bucket)
					dbg->static_sector_nodes_opt += sector_nodes;
				else
					dbg->static_sector_nodes_noopt += sector_nodes;
			}

			// Determine visibility for static geometry hierrarhy
			for (auto& pair : m_sector_frustums)
			{
				xr_vector<CFrustum>& frustums = pair.val.first;
				if (frustums.empty())
					continue;

				if (frustums.size() == 1)
				{
					if (dbg)
					{
						++dbg->static_frustum_nodes;
						++dbg->static_add_root_calls;
						if (opt_bucket)
						{
							++dbg->static_frustum_nodes_opt;
							++dbg->static_add_root_calls_opt;
						}
						else
						{
							++dbg->static_frustum_nodes_noopt;
							++dbg->static_add_root_calls_noopt;
						}
					}
#ifdef USE_DX11
					if (add_Static_parallel((IRenderVisual*)pair.key->root(), frustums.front(), frustums.front().getMask()))
						continue;
#endif
					add_Static((IRenderVisual*)pair.key->root(), frustums.front(), frustums.front().getMask());
					continue;
				}

				xr_vector<u32> masks;
				masks.reserve(frustums.size());
				for (CFrustum& frustum_node : frustums)
					masks.push_back(frustum_node.getMask());

				if (dbg)
				{
					dbg->static_frustum_nodes += u32(frustums.size());
					++dbg->static_add_root_calls;
					if (opt_bucket)
					{
						dbg->static_frustum_nodes_opt += u32(frustums.size());
						++dbg->static_add_root_calls_opt;
					}
					else
					{
						dbg->static_frustum_nodes_noopt += u32(frustums.size());
						++dbg->static_add_root_calls_noopt;
					}
				}

				add_Static_MultiFrustum((IRenderVisual*)pair.key->root(), frustums, masks);
			}
		}
	}
}

void CDSGraphManager::r_dsgraph_capture_lights()
{
	PROF_EVENT("r_dsgraph_capture_lights")
	g_SpatialSpaceLights->q_frustum
	(
		lstLights,
		ISpatial_DB::O_ORDERED,
		STYPE_LIGHTSOURCE,
		i_frustum
	);

#if	RENDER==R_R1
	std::sort(lstLights.begin(), lstLights.end(), [](const ISpatialShared& _1, const ISpatialShared& _2) noexcept
	{
		if (!_1.get() || !_2.get()) return false;

		return	_1->spatial.sphere.P.distance_to_sqr(Device.vCameraPosition_saved) < _2->spatial.sphere.P.distance_to_sqr(Device.vCameraPosition_saved);
	});
#endif

	for (ISpatialShared spatial : lstLights)
	{
		if (0 == spatial) continue;
		if (R4FullDetailRejectSpatial(spatial.get(), i_mask[CDSGraphManager::fl_normal])) continue;
		spatial->spatial_updatesector();
		CSector* sector = (CSector*)spatial->spatial.sector;
		if (0 == sector) continue;

		if (!RImplementation.HOM.visible(spatial->spatial.sphere)) continue;

		if ((spatial->spatial.type & STYPE_LIGHTSOURCE))
		{
			// lightsource
			if (light* L = (light*)(spatial->dcast_Light()))
			{
#if	RENDER==R_R1
				RImplementation.L_DB->add_light(L);
#else
				if (L->get_LOD() > ps_r2_shadow_lod_min && L->has_light_visible_from_sectors(*this))
				{
					RImplementation.Lights.add_light(L);
				}
#endif
			}
		}
	}
}

void CDSGraphManager::r_dsgraph_prefetch_dynamic()
{
	m_dynamic_prefetched = false;
	if (i_start && psDeviceFlags.test(rsDrawDynamic))
	{
		g_SpatialSpace->q_frustum(lstRenderables, ISpatial_DB::O_ORDERED, i_doptions, i_frustum);
		m_dynamic_prefetched = true;
	}
}

void CDSGraphManager::r_dsgraph_capture_dynamic(CObject* O)
{
	PROF_EVENT("r_dsgraph_capture_dynamic")
	const bool dbg_enabled = PortalTraverseDbg_Enabled();
	PortalTraverseDebugStats* dbg = dbg_enabled ? &PortalTraverseDbg_Get() : nullptr;
	const bool opt_bucket = PortalTraverseDbg_IsOptions(i_options);

	if (i_start)
	{
		if (psDeviceFlags.test(rsDrawDynamic))
		{
			// Traverse object database (unless r_dsgraph_prefetch_dynamic did)
			if (m_dynamic_prefetched)
				m_dynamic_prefetched = false;
			else
				g_SpatialSpace->q_frustum
				(
					lstRenderables,
					ISpatial_DB::O_ORDERED,
					i_doptions,
					i_frustum
				);
			if (dbg)
			{
				const u32 spatial_count = u32(lstRenderables.size());
				dbg->dynamic_spatials += spatial_count;
				if (opt_bucket)
					dbg->dynamic_spatials_opt += spatial_count;
				else
					dbg->dynamic_spatials_noopt += spatial_count;
			}
			set_Object();
#if	RENDER==R_R1
			if (i_mask[CDSGraphManager::fl_normal])//normal phase
			{
				std::sort(lstRenderables.begin(), lstRenderables.end(), [](const ISpatialShared& _1, const ISpatialShared& _2) noexcept
				{
					if (!_1.get() || !_2.get()) return false;

					return	_1->spatial.sphere.P.distance_to_sqr(Device.vCameraPosition_saved) < _2->spatial.sphere.P.distance_to_sqr(Device.vCameraPosition_saved);
				});

				if (ps_actor_shadow_flags.test(1))
					g_hud->Render_First(dcast_IPortalTraverser());

				r_dsgraph_capture_hud();
			}
#endif
			u32 uID_LTRACK = u32(-1);
			if (i_mask[CDSGraphManager::fl_normal])//normal phase
			{
				// update light-vis for current entity / actor
				if (CObject* O = g_pGameLevel->CurrentViewEntity())
				{
					if (!O->getDestroy())
					{
						if (CROS_impl* R = (CROS_impl*)O->ROS())
							R->update(O);
					}
				}

				RImplementation.uLastLTRACK++;
				if (!lstRenderables.empty())
				{
					uID_LTRACK = RImplementation.uLastLTRACK % lstRenderables.size();
#if	RENDER!=R_R1
					// update light-vis for selected entity
					// track lighting environment
					if (IRenderable* renderable = (IRenderable*)lstRenderables[uID_LTRACK]->dcast_Renderable())
					{
						if (CROS_impl* T = (CROS_impl*)renderable->renderable_ROS())
							T->update(renderable);
					}
#endif
				}

			}

			// Determine visibility for dynamic part of scene
			for (u32 o_it = 0; o_it < lstRenderables.size(); o_it++)
			{
				ISpatialShared spatial = lstRenderables[o_it];
				if (0 == spatial) continue;
				if (R4FullDetailRejectSpatial(spatial.get(), i_mask[CDSGraphManager::fl_normal])) continue;
				CSector* sector = (CSector*)spatial->spatial.sector;
				if (0 == sector) continue;

				if (i_mask[CDSGraphManager::fl_normal] && !RImplementation.HOM.visible(spatial->spatial.sphere))
					continue;
#ifdef USE_DX11
				// Hidden in a recent frame's depth (r4_hiz.h)
				if (i_mask[CDSGraphManager::fl_normal] && hiz::test_dynamic(spatial->spatial.sphere))
					continue;
#endif

#if	RENDER==R_R1
				if ((spatial->spatial.type & STYPE_GLOW))
				{
					if (CGlow* glow = spatial->dcast_CGlow())
					{
						// It may be an glow
						RImplementation.L_Glows->add(glow);
					}
					continue;
				}
#endif

//				if ((spatial->spatial.type & STYPE_LIGHTSOURCE))
//				{
//					// lightsource
//					if (light* L = (light*)(spatial->dcast_Light()))
//					{
//#if	RENDER==R_R1
//						RImplementation.L_DB->add_light(L);
//#else
//						if (L->get_LOD() > EPS_L && L->has_light_visible_from_sectors(PT))
//						{
//							RImplementation.Lights.add_light(L);
//						}
//#endif
//					}
//					continue;
//				}

				if(!(spatial->spatial.type & STYPE_RENDERABLE) && !(spatial->spatial.type & STYPE_PARTICLE) && !(spatial->spatial.type & STYPE_RENDERABLESHADOW))
					continue;
				if (!is_sector_visible(sector))
					continue;

				for (CFrustum& frustum : m_sector_frustums.find(sector)->val.first)
				{
					if (dbg)
					{
						++dbg->dynamic_frustum_tests;
						if (opt_bucket)
							++dbg->dynamic_frustum_tests_opt;
						else
							++dbg->dynamic_frustum_tests_noopt;
					}

					if (frustum.testSphere_dirty(spatial->spatial.sphere.P, spatial->spatial.sphere.R))
					{
						if (dbg)
						{
							++dbg->dynamic_frustum_hits;
							if (opt_bucket)
								++dbg->dynamic_frustum_hits_opt;
							else
								++dbg->dynamic_frustum_hits_noopt;
						}

						// renderable
						IRenderable* renderable = spatial->dcast_Renderable();
						if (0 == renderable) break;

						if (O && O->dcast_Renderable() == renderable) break;

						// Rendering
#if	RENDER==R_R1
						if (i_mask[CDSGraphManager::fl_normal] && o_it == uID_LTRACK && renderable->renderable_ROS())
						{
							// track lighting environment
							if(CROS_impl* T = (CROS_impl*)renderable->renderable_ROS())
								T->update(renderable);
						}
#endif
						if (i_mask[CDSGraphManager::fl_normal] && !(spatial->spatial.type & STYPE_PARTICLE))
							set_Object(renderable);

						renderable->renderable_Render(dcast_IPortalTraverser());
						if (dbg)
						{
							++dbg->dynamic_rendered;
							if (opt_bucket)
								++dbg->dynamic_rendered_opt;
							else
								++dbg->dynamic_rendered_noopt;
						}

						if (i_mask[CDSGraphManager::fl_normal] && !(spatial->spatial.type & STYPE_PARTICLE))
							set_Object();
						break;
					}
				}
			}
		}
	}
}

void CDSGraphManager::r_dsgraph_capture(bool lights, bool dynamic, CObject* O)
{
	PROF_EVENT("r_dsgraph_capture")
	r_dsgraph_capture_static();

	if(lights)
		r_dsgraph_capture_lights();

	if (dynamic)
		r_dsgraph_capture_dynamic(O);
}

void CDSGraphManager::r_dsgraph_render_ui_3d()
{
    const Fmatrix saved_world = RCache.get_xform_world();
    const Fmatrix saved_view = RCache.get_xform_view();
    const Fmatrix saved_project = RCache.get_xform_project();
    const Fmatrix saved_full = Device.mFullTransform;
    Device.mFullTransform = Device.mFullTransform3DIcons;
    RCache.set_xform_view(Device.mView);
    RCache.set_xform_project(Device.mProject3DIcons);
    auto render_icons = [&](auto& graph, bool material_shader = false)
    {
        std::sort(graph.begin(), graph.end());
        for (auto& item : graph)
        {
            dxRender_Visual* visual = item.pVisual;
            VERIFY(visual && visual->shader._get());
            if (material_shader)
            {
                // DX11 sorted icons use main's selected mesh material and distance-based LOD.
                RCache.set_Element(item.pSE);
                RCache.set_xform_world(*item.pMatrix);
                RImplementation.apply_object(item.pObject);
                RImplementation.apply_lmaterial();
                visual->Render(calcLOD(item.sortKey, visual->vis.sphere.R));
                continue;
            }
            R_ASSERT(visual->skinning >= -1 && visual->skinning <= 4);
            RCache.set_Shader(RImplementation.Target->s_ui_icons[visual->skinning + 1]);
            RCache.set_Textures(visual->shader->E[0]->passes[0]->T);
            RCache.set_xform_world(*item.pMatrix);
#if RENDER == R_R4
            RImplementation.apply_object(nullptr);
            RImplementation.apply_lmaterial();
#endif
            visual->Render(0.f);
        }
        graph.clear();
    };
    render_icons(RGraph.mapUI3D);
#if RENDER != R_R1
    render_icons(RGraph.mapUI3DEmissive);
#endif
#if RENDER == R_R4
    render_icons(RGraph.mapUI3DSorted, true);
#else
    render_icons(RGraph.mapUI3DSorted);
#endif
    owned_transforms.clear();
    Device.mFullTransform = saved_full;
    RCache.set_xform_world(saved_world);
    RCache.set_xform_view(saved_view);
    RCache.set_xform_project(saved_project);
}

void CDSGraphManager::r_dsgraph_render_ui()
{
	// Capture may run on workers; UI drawing touches RCache and runs here on the render thread.
	for (IRenderable* object : renderable_ui)
		object->renderable_RenderUI();
	renderable_ui.clear();
}
