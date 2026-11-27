#include "stdafx.h"

#include "fhierrarhyvisual.h"
#include "SkeletonCustom.h"
#include "../../xrEngine/fmesh.h"
#include "../../xrEngine/irenderable.h"

#include "flod.h"
#include "particlegroup.h"
#include "FTreeVisual.h"

using namespace R_dsgraph;

////////////////////////////////////////////////////////////////////////////////////////////////////
// Scene graph actual insertion and sorting ////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
float r_ssaDISCARD;
float r_ssaDONTSORT;
float r_ssaLOD_A, r_ssaLOD_B;
float r_ssaGLOD_start, r_ssaGLOD_end;
float r_ssaHZBvsTEX;

// r4_cull_freeze: the frozen view's position (objects' screen size from where the culling looks), nullptr: the camera's
const Fvector* r_cull_view_pos = nullptr;

ICF float CalcSSA(float& distSQ, Fvector& C, float R)
{
	distSQ = (r_cull_view_pos ? *r_cull_view_pos : Device.vCameraPosition).distance_to_sqr(C) + EPS;
    return (R * R / distSQ);
}

ICF float CalcSSA(float& distSQ, Fvector& C, dxRender_Visual* V)
{
    return CalcSSA(distSQ, C, V->vis.sphere.R);
}

#ifdef USE_DX11
// r4_dbg_shadow_skip: leave kinds of objects out of all shadow maps to measure their cost with stat_gpu_passes.
// 1 trees, 2 other static objects, 4 dynamic objects
extern int ps_r4_dbg_shadow_skip;
// r4_dbg_main_skip: the same for the main view (g-buffer), 1 trees, 2 other static objects, 4 dynamic objects
extern int ps_r4_dbg_main_skip;
// r4_smap_order: draw order inside a shadow map. Trees and bushes (alpha tested leaves, the bulk of its pixel work)
// go after the opaque casters, so their leaves behind a wall or the ground fail the depth test before shading;
// 2 also orders the trees front to back from the light (depth in 16 steps of the map's range), 3 orders every
// caster finely (more texture switches on the CPU). 0 keeps the shader order only.
extern int ps_r4_smap_order;
extern int ps_r4_tree_instancing;
namespace hiz { bool test_static(const Fbox& box, bool tree); }
namespace sun_receivers { bool test_caster(const Fbox& box, u32 cascade); }

#if defined(USE_DX11)
// The screen glass of WearableDevices' Promin, Vektor and Metro watch (r_dsgraph_render_HUDGlassLate)
static bool is_wearable_screen_glass(const ShaderElement* sh)
{
	if (!sh->passes.size() || !sh->passes[0]->T)
		return false;
	for (const auto& t : *sh->passes[0]->T)
	{
		if (!t.second)
			continue;
		const char* n = t.second->cName.c_str();
		if (n && !strncmp(n, "wd\\", 3) && (strstr(n, "_screen_glass") || strstr(n, "_watch_glass")))
			return true;
	}
	return false;
}
#endif
#endif

ICF bool R4FullDetailRejectStatic(dxRender_Visual* pVisual, bool normalPhase)
{
#if RENDER == R_R4
	if (ps_r4_full_detail_distance_scale >= 1.f || !normalPhase ||
		pVisual->flags.test(IRenderVisualFlags::eIgnoreOptimization) || !g_pGamePersistent ||
		!g_pGamePersistent->Environment().CurrentEnv)
		return false;

	// Keep the engine's normal LOD selection alive beyond the full-detail range.
	if (pVisual->Type == MT_LOD)
		return false;

	const float farPlane = g_pGamePersistent->Environment().CurrentEnv->far_plane;
	const float fullDetailDistance = std::max(100.f, farPlane * ps_r4_full_detail_distance_scale);
	float distanceSquared;
	const float ssa = CalcSSA(distanceSquared, pVisual->vis.sphere.P, pVisual);
	const float nearestDistance = _sqrt(distanceSquared) - pVisual->vis.sphere.R;
	if (nearestDistance <= fullDetailDistance)
		return false;

	const float transitionRange = std::max(1.f, farPlane - fullDetailDistance);
	const float transition = clampr((nearestDistance - fullDetailDistance) / transitionRange, 0.f, 1.f);
	return ssa <= r_ssaDISCARD * (1.f + transition * 11.f);
#else
	return false;
#endif
}

void CDSGraphManager::r_dsgraph_insert_dynamic(dxRender_Visual *pVisual, Fmatrix* xform)
{
#ifdef USE_DX11
	if ((ps_r4_dbg_shadow_skip & 4) && i_mask[fl_shmap])
		return;
	if ((ps_r4_dbg_main_skip & 4) && i_mask[fl_normal])
		return;
#endif

	Fvector Center;
	xform->transform_tiny(Center, pVisual->vis.sphere.P);
    // Attachments can submit the same visual at several instance transforms in one pass.
    if (!get_UI3D() && val_pObject && !val_pObject->dcast_CObject())
    {
        owned_transforms.push_back(*xform);
        xform = &owned_transforms.back();
    }
    if (get_UI3D())
    {
        VERIFY(pVisual->shader._get());
        const float distance_squared = Device.vCameraPosition.distance_to_sqr(Center);
#if RENDER == R_R4
        // Match main's DX11 icon classification and SSA test, outside world HOM culling.
        const float icon_ssa = pVisual->vis.sphere.R / (distance_squared + EPS);
        if (icon_ssa <= r_ssaDISCARD) return;
        ShaderElement* shader = RImplementation.rimp_select_sh_dynamic(pVisual, distance_squared + EPS);
        if (!shader) return;
#endif
        owned_transforms.push_back(*xform);
        Fmatrix* icon_transform = &owned_transforms.back();
#if RENDER == R_R4
        if (shader->flags.bStrictB2F)
            RGraph.mapUI3DSorted.emplace_back(distance_squared + EPS, icon_ssa, nullptr, pVisual, icon_transform, shader, false);
        else
        {
            RGraph.mapUI3D.emplace_back(distance_squared + EPS, icon_ssa, nullptr, pVisual, icon_transform, shader, false);
            if (shader->flags.bEmissive)
                RGraph.mapUI3DEmissive.emplace_back(distance_squared + EPS, icon_ssa, nullptr, pVisual,
                    icon_transform, pVisual->shader->E[4]._get(), false);
        }
#else
        // DX8/9/10 use the dedicated icon shader for every mesh, far to near.
        RGraph.mapUI3DSorted.emplace_back(distance_squared, 1.f, nullptr, pVisual, icon_transform, nullptr, false);
#endif
        return;
    }


	float distSQ;
	float SSA = CalcSSA(distSQ, Center, pVisual);
    Flags16& flags = pVisual->flags;
    ShaderElement* sh_d = &*pVisual->shader->E[4];
    if (!(flags.test(IRenderVisualFlags::eIgnoreOptimization) || (sh_d && sh_d->flags.bEmissive)))
    {
        if (SSA < r_ssaDISCARD)
        {
            //Msg("SSA %.2f discarded", SSA);
            return;
        }
    }
    

	// HOM occlusion culling for dynamic objects
    if (ps_r__common_flags.test(RFLAG_HOM_DYNAMIC))
    {
#if RENDER!=R_R1
        if (i_mask[CDSGraphManager::fl_normal])
#endif
        {
            Fbox world_bb;
            world_bb.xform(pVisual->vis.box, *xform);
            if (!RImplementation.HOM.visible(world_bb))
                return;
        }
    }

	// Distortive geometry should be marked and R2 special-cases it
	// a) Allow to optimize RT order
	// b) Should be rendered to special distort buffer in another pass
	VERIFY(pVisual->shader._get());

	if (sh_d && sh_d->flags.bDistort && i_mask[sh_d->flags.iPriority/2])
	{
		if (val_fCustomFOV != 0.f)
			RGraph.mapCustomFOVSorted.Distort.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh_d, false, val_fCustomFOV);
		else if (i_mask[CDSGraphManager::fl_hud])
			RGraph.mapHUDSorted.Distort.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh_d, i_mask[CDSGraphManager::fl_hud]);
		else
			RGraph.mapDynamicSorted.Distort.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh_d, i_mask[CDSGraphManager::fl_hud]);
	}

	// Select shader
	ShaderElement* sh = RImplementation.rimp_select_sh_dynamic(pVisual, distSQ);

	if (0==sh)
		return;
	u32 shader_priority = sh->flags.iPriority/2;
	if (!i_mask[shader_priority])
		return;

	// Create common node
	// NOTE: Invisible elements exist only in R1

#if defined(USE_DX11) //  Redotix99: for 3D Shader Based Scopes 		
	switch (sh->flags.iScopeLense) {	
		case 0:
			break;

		case 1: {
			(get_CamAttached() ? RGraph.mapCamAttached : RGraph.mapHUD).emplace_back(EPS, SSA, val_pObject, pVisual, xform, sh, get_HUD(), val_fCustomFOV);

			// SSS: Deprecated
			/*if (!sh->passes[0]->ps->hud_disabled)
			{
				HUDMask_Node* N2 = HUDMask.insertInAnyWay(EPS);
				N2->val.ssa = SSA;
				N2->val.pObject = RI.val_pObject;
				N2->val.pVisual = pVisual;
				N2->val.Matrix = *RI.val_pTransform;
				N2->val.se = sh;
			}*/
			return;
		}

		case 2: {
			(get_CamAttached() ? RGraph.mapScopeCamAttached : RGraph.mapScopeHUD).emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, get_HUD(), val_fCustomFOV);
			return;
		}

		case 3: {
			if (get_CamAttached())
				RGraph.mapScopeCamAttachedSorted.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, get_HUD(), val_fCustomFOV);
			else
				RGraph.mapScopeHUDSorted.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, get_HUD(), val_fCustomFOV);
			return;
		}
	}
#endif
    if (get_HUD() && val_fCustomFOV != 0.f)
    {
        if (sh->flags.bStrictB2F)
            RGraph.mapCustomFOVSorted.Sorted.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, false, val_fCustomFOV);
        else
            RGraph.mapCustomFOV.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, false, val_fCustomFOV);
#if RENDER != R_R1
        if (sh->flags.bEmissive)
            RGraph.mapCustomFOVSorted.Emissive.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh_d, false, val_fCustomFOV);
#endif
        return;
    }

	// HUD rendering
	if (i_mask[CDSGraphManager::fl_hud])
	{
		if (sh->flags.bStrictB2F)
		{
#if RENDER!=R_R1
			if (sh->flags.bEmissive)
			{
				if (i_mask[CDSGraphManager::fl_cam])
					RGraph.mapCamAttachedSorted.Emissive.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh_d, i_mask[CDSGraphManager::fl_cam]);
				else
					RGraph.mapHUDSorted.Emissive.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh_d, i_mask[CDSGraphManager::fl_hud]);
			}
#endif // RENDER!=R_R1
#if defined(USE_DX11)
			// The PDA screen (SSS's pda_overlay) after the upscaler, at output resolution (phase_upscale)
			if (RImplementation.upscale_pda_late && !i_mask[CDSGraphManager::fl_cam] && sh->passes.size() &&
				sh->passes[0]->ps._get() && strstr(sh->passes[0]->ps->cName.c_str(), "pda_overlay"))
			{
				RGraph.mapPDALate.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, i_mask[CDSGraphManager::fl_hud]);
				return;
			}
			// WearableDevices screen glass: with the upscaler on, the screens' UI (RenderActiveItemUI) draws after the
			// upscaler, so glass drawn in the scene lay under it. Held back to draw over it (r_dsgraph_render_HUDGlassLate)
			if (RImplementation.upscale_scene_active && !i_mask[CDSGraphManager::fl_cam] && is_wearable_screen_glass(sh))
			{
				RGraph.mapHUDGlassLate.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, i_mask[CDSGraphManager::fl_hud]);
				return;
			}
#endif
			if (i_mask[CDSGraphManager::fl_cam])
				RGraph.mapCamAttachedSorted.Sorted.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, i_mask[CDSGraphManager::fl_cam]);
			else
				RGraph.mapHUDSorted.Sorted.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, i_mask[CDSGraphManager::fl_hud]);
			return;
		}
		else
		{
			if (i_mask[CDSGraphManager::fl_cam])
				RGraph.mapCamAttached.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, i_mask[CDSGraphManager::fl_cam]);
			else
				RGraph.mapHUD.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, i_mask[CDSGraphManager::fl_hud]);

			/*
#if RENDER==R_R4
			if (RImplementation.o.ssfx_core && !sh->passes[0]->ps->hud_disabled)
			{
				HUDMask_Node* N2 = val_bCamAttached ? HUDMaskCamAttached.insertInAnyWay(distSQ) : HUDMask.insertInAnyWay(distSQ);
				N2->val.ssa = SSA;
				N2->val.pObject = RI.val_pObject;
				N2->val.pVisual = pVisual;
				N2->val.Matrix = *RI.val_pTransform;
				N2->val.se = sh;
			}
#endif*/

#if RENDER!=R_R1
			if (sh->flags.bEmissive)
			{
				if (i_mask[CDSGraphManager::fl_cam])
					RGraph.mapCamAttachedSorted.Emissive.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh_d, i_mask[CDSGraphManager::fl_cam]);
				else
					RGraph.mapHUDSorted.Emissive.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh_d, i_mask[CDSGraphManager::fl_hud]);
			}
				
#endif	//	RENDER!=R_R1

			return;
		}
	}

	// Shadows registering
#if RENDER==R_R1
	DSGraphItem<u32, false> item = { 0, SSA, val_pObject, pVisual, xform, nullptr, get_HUD(), val_fCustomFOV };
	R_dsgraph::mapDSGraphItemsMap<u32, false>::TNode N = { 0, item };
	RImplementation.L_Shadows->add_element(N);
#endif
	if (i_mask[CDSGraphManager::fl_invisible])
		return;

	// strict-sorting selection
	if (sh->flags.bStrictB2F && !pVisual->dcast_ParticleCustom())
	{
		auto& graph = val_fCustomFOV != 0.f ? RGraph.mapCustomFOVSorted.Sorted : RGraph.mapDynamicSorted.Sorted;
		graph.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, get_HUD(), val_fCustomFOV);
		return;
	}

#if RENDER!=R_R1
	// Emissive geometry should be marked and R2 special-cases it
	// a) Allow to skeep already lit pixels
	// b) Allow to make them 100% lit and really bright
	// c) Should not cast shadows
	// d) Should be rendered to accumulation buffer in the second pass
	if (sh->flags.bEmissive)
	{
		auto& graph = val_fCustomFOV != 0.f ? RGraph.mapCustomFOVSorted.Emissive : RGraph.mapDynamicSorted.Emissive;
		graph.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh_d, get_HUD(), val_fCustomFOV);
	}

	if (sh->flags.bWmark && i_mask[CDSGraphManager::fl_wmarks])
	{
		if (i_mask[CDSGraphManager::fl_hud])
			RGraph.mapHUDSorted.Wmark.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, get_HUD(), val_fCustomFOV);
		else
			RGraph.mapDynamicSorted.Wmark.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, get_HUD(), val_fCustomFOV);
		return;
	}
#endif

	for (u32 iPass = 0; iPass < sh->passes.size(); ++iPass)
	{
		// the most common node
		
		if (sh->passes[iPass] == nullptr)
		{
			continue;
		}

		SPass& pass = *sh->passes[iPass];

#if RENDER==R_R1
		AddToRenderQueue(RGraph.mapDynamicPasses[shader_priority][iPass], item, pass);
#else
		AddToRenderQueue(RGraph.mapDynamicPasses[shader_priority][iPass], { 0, SSA, val_pObject, pVisual, xform, nullptr, get_HUD(), val_fCustomFOV }, pass);
#endif
	}
}

extern float ps_r__ssaDISCARD_exp;
extern float ps_r__ssaDISCARD_fade_k;
void CDSGraphManager::r_dsgraph_insert_static(dxRender_Visual *pVisual, bool prefiltered)
{
#ifdef USE_DX11
	if ((ps_r4_dbg_shadow_skip && i_mask[fl_shmap]) || (ps_r4_dbg_main_skip && i_mask[fl_normal]))
	{
		const bool tree = pVisual->Type == MT_TREE_ST || pVisual->Type == MT_TREE_PM;
		const int skip = i_mask[fl_shmap] ? ps_r4_dbg_shadow_skip : ps_r4_dbg_main_skip;
		if (skip & (tree ? 1 : 2))
			return;
	}
#endif

#ifdef USE_DX11
	// Sun cascade: its shadow falls on nothing the view sees (r4_sun_receivers.h)
	if (i_sun_cascade >= 0 && sun_receivers::test_caster(pVisual->vis.box, u32(i_sun_cascade)))
		return;

	if (i_static_filter && (pVisual->Type == MT_TREE_ST || pVisual->Type == MT_TREE_PM) != (i_static_filter == 2))
		return;

	if (pVisual->Type == MT_TREE_ST || pVisual->Type == MT_TREE_PM)
		i_static_trees++;
#endif

	if (m_static_seen.find(pVisual) != m_static_seen.end())
	{
		if (PortalTraverseDbg_Enabled())
		{
			PortalTraverseDebugStats& dbg = PortalTraverseDbg_Get();
			const bool opt_bucket = PortalTraverseDbg_IsOptions(i_options);
			++dbg.static_dedup_skipped;
			if (opt_bucket)
				++dbg.static_dedup_skipped_opt;
			else
				++dbg.static_dedup_skipped_noopt;
		}
		return;
	}
	m_static_seen.insert(pVisual);
	if (PortalTraverseDbg_Enabled())
	{
		PortalTraverseDebugStats& dbg = PortalTraverseDbg_Get();
		const bool opt_bucket = PortalTraverseDbg_IsOptions(i_options);
		++dbg.static_dedup_seen;
		if (opt_bucket)
			++dbg.static_dedup_seen_opt;
		else
			++dbg.static_dedup_seen_noopt;
	}

	float distSQ;
	float SSA = CalcSSA(distSQ, pVisual->vis.sphere.P, pVisual);
    Flags16& flags = pVisual->flags;
    ShaderElement* sh_d = &*pVisual->shader->E[4];
    // prefiltered: prefilter_static made the size and depth tests below already
    if (!prefiltered && !(flags.test(IRenderVisualFlags::eIgnoreOptimization) || (sh_d && sh_d->flags.bEmissive)))
    {
        if (SSA < r_ssaDISCARD)
            return;

        // demonized: Replace hard cutoff with gradient cutoff
        // Smaller objects that fail the SSA test will still render depending on how much smaller they are than the discard limit.
        // Reduces the "rendering radius" effect and makes pop-in less noticeable
        // Allows to increase the discard limit for better performance without making pop-in much worse
        // Define where the "thinning" begins. 
        // E.g., objects 4x the size of the discard limit start fading.
        float fade_start = r_ssaDISCARD * ps_r__ssaDISCARD_fade_k;

        // The Gradient Zone
        if (SSA < fade_start)
        {
            // Calculate a linear survival probability between 0.0 and 1.0
            float survival_chance = (SSA - r_ssaDISCARD) / (fade_start - r_ssaDISCARD);

            // Convert the 32-bit hash to a float between 0.0 and 1.0
            // Multiplying by 1.0 / 2^32 is faster than float division
            u32 hash = GetFvectorHash(pVisual->vis.sphere.P);
            constexpr float hash_to_float = 1.0f / 4294967296.0f;
            float val = hash * hash_to_float;

            // If the object's hash value is higher than its survival chance, cull it
            if (val > _powf(survival_chance, ps_r__ssaDISCARD_exp))
                return;
        }
    }

#ifdef USE_DX11
	// Hidden in a recent frame's depth (r4_hiz.h)
	if (!prefiltered && i_mask[fl_normal] && hiz::test_static(pVisual->vis.box, pVisual->Type == MT_TREE_ST || pVisual->Type == MT_TREE_PM))
		return;
#endif

	// Distortive geometry should be marked and R2 special-cases it
	// a) Allow to optimize RT order
	// b) Should be rendered to special distort buffer in another pass
	VERIFY(pVisual->shader._get());
	if (sh_d && sh_d->flags.bDistort && i_mask[sh_d->flags.iPriority/2])
		RGraph.mapStaticSorted.Distort.emplace_back(distSQ, SSA, nullptr, pVisual, &Fidentity, sh_d, false);

	// Select shader
	ShaderElement* sh = RImplementation.rimp_select_sh_static(pVisual, distSQ);

	if (0 == sh)
		return;
	u32 shader_priority = sh->flags.iPriority / 2;
	if (!i_mask[shader_priority])
		return;

	// Water rendering
#if RENDER==R_R4
	if (sh->flags.isWater && RImplementation.o.ssfx_water)
	{
		RGraph.mapWater.emplace_back(distSQ, SSA, nullptr, pVisual, &Fidentity, sh, false);
		return;
	}
#endif

	// strict-sorting selection
	if (sh->flags.bStrictB2F)
	{
		RGraph.mapStaticSorted.Sorted.emplace_back(distSQ, SSA, nullptr, pVisual, &Fidentity, sh, false);
		return;
	}

#if RENDER!=R_R1
	// Emissive geometry should be marked and R2 special-cases it
	// a) Allow to skeep already lit pixels
	// b) Allow to make them 100% lit and really bright
	// c) Should not cast shadows
	// d) Should be rendered to accumulation buffer in the second pass
	if (sh->flags.bEmissive)
		RGraph.mapStaticSorted.Emissive.emplace_back(distSQ, SSA, nullptr, pVisual, &Fidentity, sh_d, false );

	if (sh->flags.bWmark && i_mask[CDSGraphManager::fl_wmarks])
	{
		RGraph.mapStaticSorted.Wmark.emplace_back(distSQ, SSA, nullptr, pVisual, &Fidentity, sh, false);
		return;
	}
#endif

	u32 order = 0; // RenderPacket: draw order hint, shadow maps only
#ifdef USE_DX11
	if (i_mask[fl_shmap] && ps_r4_smap_order)
	{
		const bool tree = pVisual->Type == MT_TREE_ST || pVisual->Type == MT_TREE_PM;
		if (tree)
			order |= 0x80000000u;
		if (ps_r4_smap_order >= 3 || (tree && ps_r4_smap_order == 2))
		{
			Fvector p;
			i_mXFORM.transform(p, pVisual->vis.sphere.P);
			u32 depth = u32(clampr(p.z, 0.f, 1.f) * 65535.f);
			if (ps_r4_smap_order == 2)
				depth &= 0xF000;
			order |= depth;
		}
	}
#endif

#ifdef USE_DX11
	// Tree instancing (r__dsgraph_render.cpp): the tree's geometry at its LOD (as r_dsgraph_render_graph's calcLOD), here
	// while the visual is in the cache
	u64 inst_key = 0;
	if (ps_r4_tree_instancing && (pVisual->Type == MT_TREE_ST || pVisual->Type == MT_TREE_PM))
		inst_key = static_cast<FTreeVisual*>(pVisual)->instance_key(
			_sqrt(clampr((SSA - r_ssaGLOD_end) / (r_ssaGLOD_start - r_ssaGLOD_end), 0.f, 1.f)));
#endif

	for (u32 iPass = 0; iPass < sh->passes.size(); ++iPass)
	{
		// the most common node
		if (sh->passes[iPass] == nullptr)
			continue;

		SPass& pass	= *sh->passes[iPass];

		AddToRenderQueue(RGraph.mapStaticPasses[shader_priority][iPass], { order, SSA, nullptr, pVisual, nullptr, nullptr, false }, pass);
#ifdef USE_DX11
		RGraph.mapStaticPasses[shader_priority][iPass].back().instKey = inst_key;
#endif
	}
}

#ifdef USE_DX11
namespace tex_stream { void note_draw(const STextureList* textures, float ssa); }
#endif

void CDSGraphManager::AddToRenderQueue(R_dsgraph::RenderQueue& queue, const R_dsgraph::DSGraphItem<u32, false>& item, const SPass& pass)
{
#ifdef USE_DX11
	// stat_tex_stream: main view world objects' textures and their size on screen
	if (i_mask[fl_normal] && !item.b_hud_mode)
		tex_stream::note_draw(&*pass.T, item.ssa);
#endif
	if (PortalTraverseDbg_Enabled())
	{
		PortalTraverseDebugStats& dbg = PortalTraverseDbg_Get();
		const bool opt_bucket = PortalTraverseDbg_IsOptions(i_options);
		// Static items use nullptr matrix/object in current pipeline.
		if (item.pMatrix == nullptr && item.pObject == nullptr)
		{
			++dbg.queue_static_packets;
			if (opt_bucket)
				++dbg.queue_static_packets_opt;
			else
				++dbg.queue_static_packets_noopt;
		}
		else
		{
			++dbg.queue_dynamic_packets;
			if (opt_bucket)
				++dbg.queue_dynamic_packets_opt;
			else
				++dbg.queue_dynamic_packets_noopt;
		}
	}

	queue.emplace_back(item, pass);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
void CDSGraphManager::add_Dynamic(IRenderVisual* piVisual, Fmatrix* xform)
{
	
	dxRender_Visual* pVisual = (dxRender_Visual*)piVisual;
	if (!pVisual) return;

	Flags16& flags = piVisual->flags;

	if (!i_mask[CDSGraphManager::fl_normal] && !!flags.test(IRenderVisualFlags::eNoShadow))
		return;

	// Visual is 100% visible - simply add it
	switch (pVisual->Type)
	{
	case MT_PARTICLE_GROUP:
	{
		// Add all children, doesn't perform any tests
		PS::CParticleGroup* pG = (PS::CParticleGroup*)pVisual->dcast_ParticleCustom();
		xrCriticalSectionGuard guard(&pG->onframe_lock);
		for (PS::CParticleGroup::SItem& I_ : pG->items)
		{
			add_Dynamic(I_._effect, xform);
			add_leafs_Dynamic(I_._children_related, xform);
			add_leafs_Dynamic(I_._children_free, xform);
		}
	}return;

	case MT_HIERRARHY:
	{
		// Add all children, doesn't perform any tests
		FHierrarhyVisual* pV = (FHierrarhyVisual*)pVisual;
		add_leafs_Dynamic(pV->children, xform);
	}return;

	case MT_SKELETON_ANIM:
	case MT_SKELETON_RIGID:
	{
		// Add all children, doesn't perform any tests
		CKinematics* pV = (CKinematics*)pVisual;
		BOOL _use_lod = FALSE;
		if (pV->m_lod && !get_UI3D())
		{
			Fvector Tpos;
			float D;
			xform->transform_tiny(Tpos, pV->vis.sphere.P);
			float ssa = CalcSSA(D, Tpos, pV->vis.sphere.R * 0.5f);	// assume dynamics never consume full sphere
			if (ssa < r_ssaLOD_A)
				_use_lod = TRUE;
		}
		if (_use_lod)
			add_Dynamic(pV->m_lod, xform);
		else
		{
			//pV->CalculateBones(TRUE);
			if (get_UI3D())
				pV->CalculateBones(TRUE);
			else if (i_mask[CDSGraphManager::fl_normal])
				pV->CalculateWallmarks();
			add_leafs_Dynamic(pV->children, xform);
		}
	}return;

	default:
	{
		// General type of visual
		// Calculate distance to it's center
		r_dsgraph_insert_dynamic(pVisual, xform);
	}return;
	}
}

void CDSGraphManager::add_Dynamic(dxRender_Visual* pVisual, Fmatrix* xform)
{
	if (!pVisual) return;

	Flags16& flags = pVisual->dcast_RenderVisual()->flags;

	if (!i_mask[CDSGraphManager::fl_normal] && !!flags.test(IRenderVisualFlags::eNoShadow))
		return;

	// Visual is 100% visible - simply add it
	switch (pVisual->Type)
	{
	case MT_PARTICLE_GROUP:
	{
		// Add all children, doesn't perform any tests
		PS::CParticleGroup* pG = (PS::CParticleGroup*)pVisual->dcast_ParticleCustom();
		xrCriticalSectionGuard guard(&pG->onframe_lock);
		for (PS::CParticleGroup::SItem& I_ : pG->items)
		{
			add_Dynamic(I_._effect, xform);
			add_leafs_Dynamic(I_._children_related, xform);
			add_leafs_Dynamic(I_._children_free, xform);
		}
	}break;

	case MT_HIERRARHY:
	{
		// Add all children, doesn't perform any tests
		FHierrarhyVisual* pV = (FHierrarhyVisual*)pVisual;
		add_leafs_Dynamic(pV->children, xform);
	}break;

	case MT_SKELETON_ANIM:
	case MT_SKELETON_RIGID:
	{
		// Add all children, doesn't perform any tests
		CKinematics* pV = (CKinematics*)pVisual;
		BOOL _use_lod = FALSE;
		if (pV->m_lod && !get_UI3D())
		{
			Fvector Tpos;
			float D;
			xform->transform_tiny(Tpos, pV->vis.sphere.P);
			float ssa = CalcSSA(D, Tpos, pV->vis.sphere.R * 0.5f);	// assume dynamics never consume full sphere
			if (ssa < r_ssaLOD_A)
				_use_lod = TRUE;
		}
		if (_use_lod)
			add_Dynamic(pV->m_lod, xform);
		else
		{
			//pV->CalculateBones(TRUE);
			if (get_UI3D())
				pV->CalculateBones(TRUE);
			else if (i_mask[CDSGraphManager::fl_normal])
				pV->CalculateWallmarks();
			add_leafs_Dynamic(pV->children, xform);
		}
	}break;

	default:
	{
		// General type of visual
		// Calculate distance to it's center
		r_dsgraph_insert_dynamic(pVisual, xform);
	}break;
	}
}

void CDSGraphManager::add_leafs_Dynamic(xr_vector<dxRender_Visual*>& children, Fmatrix* xform)
{
	for (dxRender_Visual* pVisual : children)
	{
		add_Dynamic(pVisual, xform);
	}
}

void CDSGraphManager::add_leafs_Dynamic(xr_vector<IRenderVisual*>& children, Fmatrix* xform)
{
	for (IRenderVisual* pVisual : children)
	{
		add_Dynamic(pVisual, xform);
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
void CDSGraphManager::add_Static(IRenderVisual* piVisual, CFrustum& frustum, u32 planes)
{
	dxRender_Visual* pVisual = (dxRender_Visual*)piVisual;
	if (!pVisual) return;

	Flags16& flags = piVisual->flags;
	if (!i_mask[CDSGraphManager::fl_normal] && !!flags.test(IRenderVisualFlags::eNoShadow))
		return;

	// Check frustum visibility and calculate distance to visual's center
	vis_data& vis = pVisual->vis;
	EFC_Visible VIS = frustum.testSAABB(vis.sphere.P, vis.sphere.R, vis.box.data(), planes);

	if (fcvNone == VIS)
		return;

	if (R4FullDetailRejectStatic(pVisual, i_mask[CDSGraphManager::fl_normal]))
		return;

#if RENDER!=R_R1
	if (i_mask[CDSGraphManager::fl_normal])//phase normal
#endif
		if (!RImplementation.HOM.visible(vis))
			return;

	// If we get here visual is visible or partially visible
	switch (pVisual->Type)
	{
	case MT_HIERRARHY:
	{
		// Add all children
		FHierrarhyVisual* pV = (FHierrarhyVisual*)pVisual;
		if (fcvPartial == VIS)
		{
			for (auto V : pV->children)
				add_Static(V, frustum, planes);
		}
		else
			add_leafs_Static(pV->children);
	}break;

	case MT_LOD:
	{
		FLOD* pV = (FLOD*)pVisual;
		float D;
		float ssa = CalcSSA(D, pV->vis.sphere.P, pV) * pV->lod_factor;

		if (ssa < r_ssaLOD_A)
		{
			if (ssa < r_ssaDISCARD)
				return;

			RGraph.mapLOD.emplace_back(D, ssa, nullptr, pVisual, nullptr, nullptr, false);
		}

#if RENDER!=R_R1
		if (ssa > r_ssaLOD_B || i_mask[CDSGraphManager::fl_shmap])//phase shmap
#else
		if (ssa > r_ssaLOD_B)
#endif
		{
			// Add all children, perform tests
			add_leafs_Static(pV->children);
		}
	}break;

	case MT_TREE_ST:
	case MT_TREE_PM:
	default:
	{
		// General type of visual
		r_dsgraph_insert_static(pVisual);
	}return;
	}
}

void CDSGraphManager::add_Static_MultiFrustum(IRenderVisual* piVisual, const xr_vector<CFrustum>& frustums, const xr_vector<u32>& masks)
{
	constexpr u32 FULLY_VISIBLE_MASK = u32(-1);

	dxRender_Visual* pVisual = (dxRender_Visual*)piVisual;
	if (!pVisual) return;

	Flags16& flags = piVisual->flags;
	if (!i_mask[CDSGraphManager::fl_normal] && !!flags.test(IRenderVisualFlags::eNoShadow))
		return;

	// Check visibility against all active frustums and propagate per-frustum masks.
	vis_data& vis = pVisual->vis;
	bool anyVisible = false;
	bool hasFullyVisibleFrustum = false;
	xr_vector<u32> childMasks;
	childMasks.resize(masks.size(), 0);

	for (u32 i = 0; i < masks.size(); ++i)
	{
		u32 planeMask = masks[i];
		if (!planeMask)
			continue;

		// Propagated from parent: this visual is fully visible in this frustum.
		if (planeMask == FULLY_VISIBLE_MASK)
		{
			anyVisible = true;
			hasFullyVisibleFrustum = true;
			childMasks[i] = FULLY_VISIBLE_MASK;
			continue;
		}

		EFC_Visible VIS = frustums[i].testSAABB(vis.sphere.P, vis.sphere.R, vis.box.data(), planeMask);
		if (VIS == fcvNone)
		{
			continue;
		}

		anyVisible = true;
		if (VIS == fcvFully)
		{
			// In original per-frustum traversal this path immediately adds leafs.
			// Preserve union semantics by marking this frustum as fully visible.
			hasFullyVisibleFrustum = true;
			childMasks[i] = FULLY_VISIBLE_MASK;
		}
		else
		{
			childMasks[i] = planeMask;
		}
	}

	if (!anyVisible)
		return;

	if (R4FullDetailRejectStatic(pVisual, i_mask[CDSGraphManager::fl_normal]))
		return;

#if RENDER!=R_R1
	if (i_mask[CDSGraphManager::fl_normal])//phase normal
#endif
		if (!RImplementation.HOM.visible(vis))
			return;

	// If we get here visual is visible in at least one frustum
	switch (pVisual->Type)
	{
	case MT_HIERRARHY:
	{
		FHierrarhyVisual* pV = (FHierrarhyVisual*)pVisual;
		if (hasFullyVisibleFrustum)
			add_leafs_Static(pV->children);
		else
		{
			for (auto V : pV->children)
				add_Static_MultiFrustum(V, frustums, childMasks);
		}
	}break;

	case MT_LOD:
	{
		FLOD* pV = (FLOD*)pVisual;
		float D;
		float ssa = CalcSSA(D, pV->vis.sphere.P, pV) * pV->lod_factor;

		if (ssa < r_ssaLOD_A)
		{
			if (ssa < r_ssaDISCARD)
				return;

			RGraph.mapLOD.emplace_back(D, ssa, nullptr, pVisual, nullptr, nullptr, false);
		}

#if RENDER!=R_R1
		if (ssa > r_ssaLOD_B || i_mask[CDSGraphManager::fl_shmap])//phase shmap
#else
		if (ssa > r_ssaLOD_B)
#endif
		{
			add_leafs_Static(pV->children);
		}
	}break;

	case MT_TREE_ST:
	case MT_TREE_PM:
	default:
	{
		r_dsgraph_insert_static(pVisual);
	}return;
	}
}

void CDSGraphManager::add_leaf_Static(dxRender_Visual* pVisual)
{
	if (R4FullDetailRejectStatic(pVisual, i_mask[CDSGraphManager::fl_normal]))
		return;

#if RENDER!=R_R1
	if (i_mask[CDSGraphManager::fl_normal])//phase normal
#endif
		if (!RImplementation.HOM.visible(pVisual->vis))
			return;

	// Visual is 100% visible - simply add it
	switch (pVisual->Type)
	{
	case MT_HIERRARHY:
	{
		FHierrarhyVisual* pV = (FHierrarhyVisual*)pVisual;
		add_leafs_Static(pV->children);
	}break;

	case MT_LOD:
	{
		FLOD* pV = (FLOD*)pVisual;
		float D;
		float ssa = CalcSSA(D, pV->vis.sphere.P, pV) * pV->lod_factor;

		if (ssa < r_ssaLOD_A)
		{
			if (ssa < r_ssaDISCARD)
				break;

			RGraph.mapLOD.emplace_back(D, ssa, nullptr, pVisual, nullptr, nullptr, false);
		}

#if RENDER!=R_R1
		if (ssa > r_ssaLOD_B || i_mask[CDSGraphManager::fl_shmap])//phase shmap
#else
		if (ssa > r_ssaLOD_B)
#endif
		{
			// Add all children, doesn't perform any tests
			add_leafs_Static(pV->children);
		}
	}break;

	case MT_TREE_PM:
	case MT_TREE_ST:
	default:
	{
		// General type of visual
		r_dsgraph_insert_static(pVisual);
	}break;
	}
}

#ifdef USE_DX11
// On by default with 12 hardware threads or more: on the Steam Deck's 4 cores (8 threads) the workers compete with the
// graphics driver's threads and the frame got 0.1-0.4 ms longer
int ps_r4_main_parallel_cull = std::thread::hardware_concurrency() >= 12 ? 1 : 0;
// HOM's recheck delay on the worker threads (::Random is not thread-safe)
static thread_local CRandom s_static_rnd{ s32(GetCurrentThreadId()) };

// r_dsgraph_insert_static's rejects that don't depend on the graph (object size, depth of a recent frame): the same
// tests in the same order, so a visual it passes is one r_dsgraph_insert_static would not reject for these reasons
bool CDSGraphManager::prefilter_static(dxRender_Visual* pVisual)
{
	float distSQ;
	const float SSA = CalcSSA(distSQ, pVisual->vis.sphere.P, pVisual);
	Flags16& flags = pVisual->flags;
	ShaderElement* sh_d = &*pVisual->shader->E[4];
	if (!(flags.test(IRenderVisualFlags::eIgnoreOptimization) || (sh_d && sh_d->flags.bEmissive)))
	{
		if (SSA < r_ssaDISCARD)
			return false;
		const float fade_start = r_ssaDISCARD * ps_r__ssaDISCARD_fade_k;
		if (SSA < fade_start)
		{
			const float survival_chance = (SSA - r_ssaDISCARD) / (fade_start - r_ssaDISCARD);
			const u32 hash = GetFvectorHash(pVisual->vis.sphere.P);
			constexpr float hash_to_float = 1.0f / 4294967296.0f;
			const float val = hash * hash_to_float;
			if (val > _powf(survival_chance, ps_r__ssaDISCARD_exp))
				return false;
		}
	}
	if (hiz::test_static(pVisual->vis.box, pVisual->Type == MT_TREE_ST || pVisual->Type == MT_TREE_PM))
		return false;
	return true;
}

// add_Static for the main view (fl_normal), collecting instead of inserting
void CDSGraphManager::collect_Static(dxRender_Visual* pVisual, const CFrustum& frustum, u32 planes, xr_vector<static_item>& out, CRandom& rnd)
{
	if (!pVisual)
		return;
	vis_data& vis = pVisual->vis;
	const EFC_Visible VIS = frustum.testSAABB(vis.sphere.P, vis.sphere.R, vis.box.data(), planes);
	if (fcvNone == VIS)
		return;
	if (!RImplementation.HOM.visible(vis, rnd))
		return;

	switch (pVisual->Type)
	{
	case MT_HIERRARHY:
	{
		FHierrarhyVisual* pV = (FHierrarhyVisual*)pVisual;
		if (fcvPartial == VIS)
		{
			for (auto V : pV->children)
				collect_Static((dxRender_Visual*)V, frustum, planes, out, rnd);
		}
		else
		{
			for (auto V : pV->children)
				collect_leaf_Static((dxRender_Visual*)V, out, rnd);
		}
	}break;

	case MT_LOD:
	{
		FLOD* pV = (FLOD*)pVisual;
		float D;
		const float ssa = CalcSSA(D, pV->vis.sphere.P, pV) * pV->lod_factor;
		if (ssa < r_ssaLOD_A)
		{
			if (ssa < r_ssaDISCARD)
				return;
			out.push_back({ pVisual, D, ssa, true });
		}
		if (ssa > r_ssaLOD_B)
		{
			for (auto V : pV->children)
				collect_leaf_Static((dxRender_Visual*)V, out, rnd);
		}
	}break;

	default:
		if (prefilter_static(pVisual))
			out.push_back({ pVisual, 0.f, 0.f, false });
	}
}

// add_leaf_Static for the main view, collecting instead of inserting
void CDSGraphManager::collect_leaf_Static(dxRender_Visual* pVisual, xr_vector<static_item>& out, CRandom& rnd)
{
	if (!pVisual)
		return;
	if (!RImplementation.HOM.visible(pVisual->vis, rnd))
		return;

	switch (pVisual->Type)
	{
	case MT_HIERRARHY:
	{
		FHierrarhyVisual* pV = (FHierrarhyVisual*)pVisual;
		for (auto V : pV->children)
			collect_leaf_Static((dxRender_Visual*)V, out, rnd);
	}break;

	case MT_LOD:
	{
		FLOD* pV = (FLOD*)pVisual;
		float D;
		const float ssa = CalcSSA(D, pV->vis.sphere.P, pV) * pV->lod_factor;
		if (ssa < r_ssaLOD_A)
		{
			if (ssa < r_ssaDISCARD)
				break;
			out.push_back({ pVisual, D, ssa, true });
		}
		if (ssa > r_ssaLOD_B)
		{
			for (auto V : pV->children)
				collect_leaf_Static((dxRender_Visual*)V, out, rnd);
		}
	}break;

	default:
		if (prefilter_static(pVisual))
			out.push_back({ pVisual, 0.f, 0.f, false });
	}
}

// The top two levels of the hierarchy tested here, as add_Static does; what is below them is collected in parallel,
// one list per subtree, and inserted afterwards in add_Static's order. False: not done (not the main view)
bool CDSGraphManager::add_Static_parallel(IRenderVisual* piRoot, CFrustum& frustum, u32 planes)
{
	if (!ps_r4_main_parallel_cull || !i_mask[fl_normal] || i_mask[fl_shmap] || i_sun_cascade >= 0)
		return false;

	struct work
	{
		dxRender_Visual* visual;
		u32 planes;
		bool leaf; // a child of a fully visible hierarchy: add_leaf_Static, else add_Static
	};
	static xr_vector<work> works;
	static xr_vector<xr_vector<static_item>> results;
	works.clear();

	struct splitter
	{
		const CFrustum& frustum;
		void split(dxRender_Visual* pVisual, u32 planes, int depth)
		{
			if (!pVisual)
				return;
			if (depth >= 2 || pVisual->Type != MT_HIERRARHY)
			{
				works.push_back({ pVisual, planes, false });
				return;
			}
			vis_data& vis = pVisual->vis;
			const EFC_Visible VIS = frustum.testSAABB(vis.sphere.P, vis.sphere.R, vis.box.data(), planes);
			if (fcvNone == VIS)
				return;
			if (!RImplementation.HOM.visible(vis))
				return;
			FHierrarhyVisual* pV = (FHierrarhyVisual*)pVisual;
			for (auto V : pV->children)
			{
				if (fcvPartial == VIS)
					split((dxRender_Visual*)V, planes, depth + 1);
				else if (V)
					works.push_back({ (dxRender_Visual*)V, 0, true });
			}
		}
	} splitter_{ frustum };
	splitter_.split((dxRender_Visual*)piRoot, planes, 0);

	const u32 count = u32(works.size());
	if (results.size() < count)
		results.resize(count);
	{
		PROF_EVENT("main view: parallel static visibility");
		xr_parallel_for(0u, count, [this, &frustum](u32 i)
		{
			CRandom& rnd = s_static_rnd;
			xr_vector<static_item>& out = results[i];
			out.clear();
			const work& w = works[i];
			if (w.leaf)
				collect_leaf_Static(w.visual, out, rnd);
			else
				collect_Static(w.visual, frustum, w.planes, out, rnd);
		});
	}
	{
		PROF_EVENT("main view: static insert");
		for (u32 i = 0; i < count; ++i)
		{
			for (const static_item& it : results[i])
			{
				if (it.lod)
					RGraph.mapLOD.emplace_back(it.D, it.ssa, nullptr, it.visual, nullptr, nullptr, false);
				else
					r_dsgraph_insert_static(it.visual, true);
			}
		}
	}
	return true;
}
#endif

void CDSGraphManager::add_leafs_Static(xr_vector<dxRender_Visual*>& children)
{
	for (dxRender_Visual* pVisual : children)
	{
		add_leaf_Static(pVisual);
	}
}

void CDSGraphManager::add_leafs_Static(xr_vector<IRenderVisual*>& children)
{
	for (IRenderVisual* pVisual : children)
	{
		dxRender_Visual* pV = (dxRender_Visual*)pVisual;
		if (!pV) continue;
		add_leaf_Static(pV);
	}
}
