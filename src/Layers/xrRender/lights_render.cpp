#include "stdafx.h"
#include "../../xrEngine/xr_object.h"
#include "FBasicVisual.h"
#include "SkeletonCustom.h"
#include "light_smap_keep.h"
#include "../xrRenderPC_R4/r4_vrs.h"

extern int ps_r2_shadow_omnipart_vischeck;

#ifdef USE_DX11
// Lamp shadow maps kept between frames, see light_smap_keep.h
namespace light_smap_keep
{
	int ps_keep = 1;
	int ps_keep_trees = 20; // r4_light_smap_keep_trees: meters from the camera beyond which a lamp with trees is kept, 0 never
	int ps_keep_dynamic = 1; // r4_light_smap_keep_dynamic: dynamic casters that are where and as they were drawn are kept too
	float ps_keep_trees_refresh = 1.f; // r4_light_smap_keep_trees_refresh: seconds between redraws of such a map, 0 never

	struct entry
	{
		ID3DTexture2D* tex = nullptr;
		ID3DDepthStencilView* dsv = nullptr;
		ID3DShaderResourceView* srv = nullptr;
		u32 size = 0, bytes = 0, version = 0, used = 0, drawn = 0; // drawn: the time it was last drawn
		Fmatrix combine;
		u64 dynamic = 0; // signature of the dynamic casters drawn into it (0: none)
		u64 dynamic_now = 0; // this frame's, taken before drawing
		bool valid = false;
	};
	static xr_map<light*, entry> s_entries;
	static u32 s_tree_refresh_frame = 0; // one lamp with trees drawn again per frame
	static u32 s_bytes = 0;
	const u32 kMaxBytes = 128u << 20; // all kept maps together
	const u32 kUnusedFrames = 600;

	// stat_light_keep
	static bool s_stats = false;
	static u32 s_stats_end = 0, s_frames = 0, s_last_stat_frame = 0;
	static u32 s_kept = 0, s_kept_trees = 0, s_drawn = 0, s_off_moving = 0, s_off_dynamic = 0, s_off_trees = 0, s_off_forward = 0,
		s_off_grass = 0, s_off_other = 0, s_off_memory = 0, s_kept_dynamic = 0, s_dynamic_still = 0;

	void start_stats(float seconds)
	{
		s_stats = true;
		s_stats_end = Device.dwTimeGlobal + iFloor(seconds * 1000.f);
		s_kept = s_kept_trees = s_drawn = s_off_moving = s_off_dynamic = s_off_trees = s_off_forward = s_off_grass = s_off_other = s_off_memory = s_frames = 0;
		s_kept_dynamic = s_dynamic_still = 0;
		Msg("* [Light smap keep] measuring for %.0f s...", seconds);
	}

	static void count(u32& c)
	{
		if (!s_stats)
			return;
		if (Device.dwTimeGlobal >= s_stats_end)
		{
			s_stats = false;
			const float f = float(_max(1u, s_frames));
			Msg("* [Light smap keep] per frame over %u frames, %u maps kept (%.1f MB): shadow maps kept %.2f (with trees %.2f), drawn into their own map %.2f,"
				" drawn as usual: moving %.2f, dynamic objects %.2f, trees near %.2f, semi-transparent %.2f, grass %.2f, memory %.2f, other %.2f;"
				" with dynamic casters: kept %.2f, unchanged since the frame before %.2f",
				s_frames, u32(s_entries.size()), float(s_bytes) / (1024.f * 1024.f), s_kept / f, s_kept_trees / f, s_drawn / f, s_off_moving / f,
				s_off_dynamic / f, s_off_trees / f, s_off_forward / f, s_off_grass / f, s_off_memory / f, s_off_other / f,
				s_kept_dynamic / f, s_dynamic_still / f);
			xrLogger::FlushLog();
			return;
		}
		if (s_last_stat_frame != Device.dwFrame)
		{
			s_last_stat_frame = Device.dwFrame;
			s_frames++;
		}
		c++;
	}

	static void release(entry& e)
	{
		_RELEASE(e.srv);
		_RELEASE(e.dsv);
		_RELEASE(e.tex);
		s_bytes -= e.bytes;
		e.bytes = 0;
		e.size = 0;
		e.valid = false;
	}

	// The lamp's own texture, in the atlas' format
	static bool ensure_texture(entry& e, u32 size)
	{
		if (e.tex && e.size == size)
			return true;
		release(e);

		D3D11_TEXTURE2D_DESC atlas = {};
		((ID3DTexture2D*)RImplementation.Target->rt_smap_depth->pSurface)->GetDesc(&atlas);
		DXGI_FORMAT dsv_format, srv_format;
		u32 bpp;
		switch (atlas.Format)
		{
		case DXGI_FORMAT_R32_TYPELESS: dsv_format = DXGI_FORMAT_D32_FLOAT; srv_format = DXGI_FORMAT_R32_FLOAT; bpp = 4; break;
		case DXGI_FORMAT_R24G8_TYPELESS: dsv_format = DXGI_FORMAT_D24_UNORM_S8_UINT; srv_format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; bpp = 4; break;
		case DXGI_FORMAT_R16_TYPELESS: dsv_format = DXGI_FORMAT_D16_UNORM; srv_format = DXGI_FORMAT_R16_UNORM; bpp = 2; break;
		default: return false;
		}
		const u32 bytes = size * size * bpp;
		if (s_bytes + bytes > kMaxBytes)
			return false;

		D3D11_TEXTURE2D_DESC d = {};
		d.Width = d.Height = size;
		d.MipLevels = d.ArraySize = 1;
		d.Format = atlas.Format;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
		D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
		dv.Format = dsv_format;
		dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
		D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
		sv.Format = srv_format;
		sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		sv.Texture2D.MipLevels = 1;
		if (FAILED(HW.pDevice->CreateTexture2D(&d, nullptr, &e.tex)) || FAILED(HW.pDevice->CreateDepthStencilView(e.tex, &dv, &e.dsv)) ||
			FAILED(HW.pDevice->CreateShaderResourceView(e.tex, &sv, &e.srv)))
		{
			release(e);
			return false;
		}
		e.size = size;
		e.bytes = bytes;
		s_bytes += bytes;
		return true;
	}

	// No dynamic object captured for this lamp's shadow map
	static bool dynamic_empty(const CDSGraphManager& G)
	{
		for (u32 p = 0; p < 2; ++p)
			for (u32 i = 0; i < SHADER_PASSES_MAX; ++i)
				if (!G.RGraph.mapDynamicPasses[p][i].empty())
					return false;
		return G.RGraph.mapDynamicSorted.Sorted.empty();
	}

	// Signature of the dynamic casters captured for this lamp: each draw's visual, shader, world matrix and detail (ssa),
	// and each skeleton's bone transforms as skinning reads them. Order-independent (a sum of item hashes). 0 when none
	static u64 hash_bytes(const void* p, size_t n, u64 h) // n: a multiple of 4
	{
		const u32* w = (const u32*)p;
		for (size_t i = 0; i < n / 4; ++i)
			h = (h ^ w[i]) * 0x100000001b3ull + (h >> 29);
		return h;
	}

	static u64 dynamic_signature(const CDSGraphManager& G)
	{
		u64 sum = 0, count = 0;
		static xr_vector<CKinematics*> skeletons;
		skeletons.clear();
		for (u32 i = 0; i < SHADER_PASSES_MAX; ++i)
		{
			for (const auto& rp : G.RGraph.mapDynamicPasses[0][i])
			{
				const auto& it = rp.item;
				u64 h = 14695981039346656037ull;
				h = hash_bytes(&it.pVisual, sizeof(it.pVisual), h);
				h = hash_bytes(&it.pSE, sizeof(it.pSE), h);
				h = hash_bytes(&it.ssa, sizeof(it.ssa), h);
				if (it.pMatrix)
					h = hash_bytes(it.pMatrix, sizeof(Fmatrix), h);
				sum += h;
				++count;
				if (it.pObject && it.pObject->renderable.visual)
				{
					CKinematics* K = PCKinematics((dxRender_Visual*)it.pObject->renderable.visual);
					if (K && std::find(skeletons.begin(), skeletons.end(), K) == skeletons.end())
						skeletons.push_back(K);
				}
			}
		}
		// A skeleton's bones hashed once per frame, whatever the number of lamps it is in
		struct bones_hash
		{
			CKinematics* K;
			u32 frame;
			u64 h;
		};
		static xr_vector<bones_hash> cache;
		static u32 cache_frame = 0;
		if (cache_frame != Device.dwFrame)
		{
			cache_frame = Device.dwFrame;
			cache.clear();
		}
		for (CKinematics* K : skeletons)
		{
			u64 h = 0;
			auto found = std::find_if(cache.begin(), cache.end(), [K](const bones_hash& c) { return c.K == K; });
			if (found != cache.end())
				h = found->h;
			else
			{
				h = 1469598103934665603ull;
				h = hash_bytes(&K, sizeof(K), h);
				for (u16 b = 0; b < K->LL_BoneCount(); ++b)
					h = hash_bytes(&K->LL_GetTransform_R(b), sizeof(Fmatrix), h);
				cache.push_back({ K, cache_frame, h });
			}
			sum += h;
		}
		return count ? (sum ^ (count << 56)) | 1 : 0;
	}

	// The lamp's entry when its shadow map can be kept this frame, else nullptr. fresh: the entry already holds it
	static entry* get(light* L, bool decorative, bool forward, bool grass, bool& fresh)
	{
		fresh = false;
		if (!ps_keep)
			return nullptr;
		if (decorative || L->flags.bHudMode)
		{
			count(s_off_other);
			return nullptr;
		}
		if (L->m_moving_frames < 32u)
		{
			count(s_off_moving);
			return nullptr;
		}
		if (forward)
		{
			count(s_off_forward);
			return nullptr;
		}
		if (grass)
		{
			count(s_off_grass);
			return nullptr;
		}
		// Trees sway: a lamp with trees among its casters is kept only beyond r4_light_smap_keep_trees meters from the
		// camera, and its map is drawn again every r4_light_smap_keep_trees_refresh seconds (one such lamp per frame)
		bool trees = false;
		if (L->GMLight.i_static_trees)
		{
			const float dist = float(ps_keep_trees);
			if (dist <= 0.f || L->position.distance_to_sqr(Device.vCameraPosition) < dist * dist)
			{
				count(s_off_trees);
				return nullptr;
			}
			trees = true;
		}
		u64 dynamic = 0;
		if (!dynamic_empty(L->GMLight))
		{
			// Dynamic casters that are where and as they were when the map was drawn keep it too (an item on the floor,
			// a body): r4_light_smap_keep_dynamic. The stat counts the lamps whose casters are unchanged either way
			dynamic = dynamic_signature(L->GMLight);
			auto found = s_entries.find(L);
			const bool still = found != s_entries.end() && found->second.valid && found->second.dynamic == dynamic;
			if (still)
				count(s_dynamic_still);
			if (!ps_keep_dynamic)
			{
				count(s_off_dynamic);
				return nullptr;
			}
		}
		entry& e = s_entries[L];
		e.used = Device.dwFrame;
		const u32 size = L->X.S.size;
		fresh = e.valid && e.size == size && e.version == L->m_static_version && !memcmp(&e.combine, &L->X.S.combine, sizeof(Fmatrix)) &&
			e.dynamic == dynamic;
		e.dynamic_now = dynamic;
		if (dynamic)
			count(s_kept_dynamic);
		if (fresh && trees && ps_keep_trees_refresh > 0.f && Device.dwTimeGlobal - e.drawn >= u32(ps_keep_trees_refresh * 1000.f) &&
			s_tree_refresh_frame != Device.dwFrame)
		{
			s_tree_refresh_frame = Device.dwFrame;
			fresh = false;
		}
		if (!ensure_texture(e, size))
		{
			s_entries.erase(L);
			count(s_off_memory);
			return nullptr;
		}
		count(fresh ? (trees ? s_kept_trees : s_kept) : s_drawn);
		return &e;
	}

	static void filled(entry& e, light* L)
	{
		// Taken before drawing: bones updated by the draw itself only make the next frame draw again
		e.dynamic = e.dynamic_now;
		e.version = L->m_static_version;
		e.combine = L->X.S.combine;
		e.drawn = Device.dwTimeGlobal;
		e.valid = true;
	}

	// Entries of lamps not drawn for a while
	static void sweep()
	{
		for (auto it = s_entries.begin(); it != s_entries.end();)
		{
			if (Device.dwFrame - it->second.used > kUnusedFrames)
			{
				release(it->second);
				it = s_entries.erase(it);
			}
			else
				++it;
		}
	}

	void forget(light* L)
	{
		auto it = s_entries.find(L);
		if (it == s_entries.end())
			return;
		release(it->second);
		s_entries.erase(it);
	}

	void destroy()
	{
		for (auto& it : s_entries)
			release(it.second);
		s_entries.clear();
	}
}
#endif

bool check_grass_shadow(light* L, CFrustum VB)
{
	// Grass shadows are allowed?
	if (ps_ssfx_grass_shadows.x < 3 || !psDeviceFlags2.test(rsGrassShadow))
		return false;

	// Inside the range?
	if (L->vis.distance > ps_ssfx_grass_shadows.z)
		return false;

	// Is in view? L->vis.visible?
	u32 mask = 0xff;
	if (!VB.testSphere(L->position, L->range * 0.6f, mask))
		return false;

	return true;
}

// A lamp drawn with its decorative objects instead of the level's geometry
static bool light_has_decor(light* L)
{
	for (int f = 0; f < 6; f++)
		if (L->decor_object[f] && !L->decor_object[f]->getDestroy())
			return true;
	return false;
}

#ifdef USE_DX11
int ps_r4_lights_parallel_query = 1; // lamps that don't move: their dynamic objects' spatial queries in parallel
#endif

void CRender::render_lights(light_Package& LP)
{
#ifdef USE_DX11
	light_smap_keep::sweep();
#endif
	stats.ls_shadowed_in += (u32)LP.v_shadowed.size();
	stats.ls_unshadowed_point_in += (u32)LP.v_point.size();
	stats.ls_unshadowed_spot_in += (u32)LP.v_spot.size();
	stats.ls_shadowed_peak_in = _max(stats.ls_shadowed_peak_in, (u32)LP.v_shadowed.size());
	//////////////////////////////////////////////////////////////////////////

	{
#if defined(USE_DX10) || defined(USE_DX11)
		PIX_EVENT(SHADOWED_LIGHTS);
#endif
		{
#if defined(USE_DX10) || defined(USE_DX11)
			PIX_EVENT(PHASE_VIS_UPDATE);
#endif
			xr_vector<light*>& source = LP.v_shadowed;
			source.erase(std::remove_if(source.begin(), source.end(), [](light* L)
			{
				if(L->m_parent)
				{
					if (ps_r2_shadow_omnipart_vischeck)
					{
						L->vis_update();
					}
					else if(L->m_parent->omnipart[0] == L)
					{
						L->m_parent->vis_update();
						for (int f = 0; f < 6; f++)
						{
							L->m_parent->omnipart[f]->vis.pending = L->m_parent->vis.pending;
							L->m_parent->omnipart[f]->vis.visible = L->m_parent->vis.visible;
						}
					}
				}
				else
					L->vis_update();
				if (!L->vis.visible)
				{
					RImplementation.stats.ls_shadowed_invisible_skipped++;
					return true;
				}

				L->optimize_smap_size();

				return false;
			}), source.end());
			stats.ls_shadowed_after_vis += (u32)source.size();
			stats.ls_shadowed_peak_after_vis = _max(stats.ls_shadowed_peak_after_vis, (u32)source.size());
		}

		{
#if defined(USE_DX10) || defined(USE_DX11)
			PIX_EVENT(PHASE_CALC_POOLS);
#endif
			xr_vector<light*>& source = LP.v_shadowed;
			static xr_vector<light*> refactored;
			refactored.clear();
			u32 total = (u32)source.size();

			for (u16 smap_ID = 0; refactored.size() != total; smap_ID++)
			{
				LP_smap_pool.initialize(o.smapsize);
				std::sort(source.begin(), source.end(), [](light* _1, light* _2) {return _1->X.S.size > _2->X.S.size; });
				source.erase(std::remove_if(source.begin(), source.end(), [smap_ID](light* L)
				{
					SMAP_Rect R;
					if (RImplementation.LP_smap_pool.push(R, L->X.S.size))
					{
						L->X.S.posX = R.min.x;
						L->X.S.posY = R.min.y;
						L->vis.smap_ID = smap_ID;
						refactored.push_back(L);
						return true;
					}
					return false;
				}), source.end());
			}

			std::reverse(refactored.begin(), refactored.end());
			LP.v_shadowed = refactored;
		}

		//////////////////////////////////////////////////////////////////////////
		// sort lights by importance???
		// while (has_any_lights_that_cast_shadows) {
		//		if (has_point_shadowed)		->	generate point shadowmap
		//		if (has_spot_shadowed)		->	generate spot shadowmap
		//		switch-to-accumulator
		//		if (has_point_unshadowed)	-> 	accum point unshadowed
		//		if (has_spot_unshadowed)	-> 	accum spot unshadowed
		//		if (was_point_shadowed)		->	accum point shadowed
		//		if (was_spot_shadowed)		->	accum spot shadowed
		//	}
		//	if (left_some_lights_that_doesn't cast shadows)
		//		accumulate them
#ifdef USE_DX11
		// Lamps that don't move (their static geometry is kept): the spatial queries of their dynamic objects in
		// parallel, ahead of the loop below. Each fills its lamp's own list and only reads the spatial database
		if (ps_r4_lights_parallel_query)
		{
			PROF_EVENT("Lights: parallel dynamic queries");
			static xr_vector<light*> query;
			query.clear();
			for (light* L : LP.v_shadowed)
				if (L->m_moving_frames >= 32u && !light_has_decor(L))
					query.push_back(L);
			xr_parallel_for(0u, u32(query.size()), [](u32 i) { query[i]->GMLight.r_dsgraph_prefetch_dynamic(); });
		}
#endif
		while (!LP.v_shadowed.empty())
		{
			// if (has_spot_shadowed)
			static xr_vector<light*> L_spot_s;
			{
#if defined(USE_DX10) || defined(USE_DX11)
				PIX_EVENT(GENERATE_SHMAPS);
#endif
				// generate spot shadowmap
				Target->phase_smap_spot_clear();
				xr_vector<light*>& source = LP.v_shadowed;
				light* L = source.back();
				u16			sid = L->vis.smap_ID;
				while (!source.empty())
				{
					if (source.empty())		break;
					L = source.back();
					if (L->vis.smap_ID != sid)	break;
					source.pop_back();
#ifdef USE_DX11
					L->m_smap_keep_srv = nullptr; // set below when its kept shadow map is used this frame
#endif
					// render
					phase = PHASE_SMAP;
#if defined(USE_DX10) || defined(USE_DX11)
					PIX_EVENT(RENDER_SHADOWS);
#endif
					bool decorative_light = false;
					{
						if ((L->decor_object[0] && !L->decor_object[0]->getDestroy()) || (L->decor_object[1] && !L->decor_object[1]->getDestroy()) || (L->decor_object[2] && !L->decor_object[2]->getDestroy()) || (L->decor_object[3] && !L->decor_object[3]->getDestroy()) || (L->decor_object[4] && !L->decor_object[4]->getDestroy()) || (L->decor_object[5] && !L->decor_object[5]->getDestroy()))
						{
							for (int f = 0; f < 6; f++)
							{
								if (L->decor_object[f] && !L->decor_object[f]->getDestroy())
								{
									L->decor_object[f]->renderable_Render(&L->GMLight);
									decorative_light = true;
								}
							}
							L->GMLight.m_dynamic_prefetched = false;
						}
						else
						{
							if (L->m_moving_frames<32u)
							{
								L->GMLight.m_dynamic_prefetched = false; // moved since: queried after the traverse
								L->GMLight.RGraph.clear_static<false>();
								L->GMLight.i_static_trees = 0;
								L->GMLight.traverse((CSector*)L->SpatialComponent->spatial.sector, L->X.S.frustum, L->position, L->X.S.combine);
								L->GMLight.r_dsgraph_capture_static();
								L->m_moving_frames++;
								L->m_static_version++;
							}
							L->GMLight.r_dsgraph_capture_dynamic(L->ignore_object);
						}
					}

					bool bDeffered_Shadows = L->GMLight.RGraph.mapStaticPasses[0][0].size() || L->GMLight.RGraph.mapDynamicPasses[0][0].size();
					bool bForward_Shadows = L->GMLight.RGraph.mapStaticPasses[1][0].size() || L->GMLight.RGraph.mapDynamicPasses[1][0].size() || L->GMLight.RGraph.mapStaticSorted.Sorted.size() || L->GMLight.RGraph.mapDynamicSorted.Sorted.size();
					if (bDeffered_Shadows || bForward_Shadows)
					{
						L_spot_s.push_back(L);
#ifdef USE_DX11
						// Kept shadow map (light_smap_keep): the lamp's own texture, drawn only when something changed
						const bool grass = Details && Details->dtFS && check_grass_shadow(L, ViewBase) && L->flags.bShadow && !decorative_light;
						bool keep_fresh = false;
						if (light_smap_keep::entry* keep = light_smap_keep::get(L, decorative_light, bForward_Shadows, grass, keep_fresh))
						{
							if (!keep_fresh)
							{
								Target->phase_smap_keep(keep->dsv, L->X.S.size);
								RCache.set_xform_world(Fidentity);
								RCache.set_xform_view(L->X.S.view);
								RCache.set_xform_project(L->X.S.project);
								L->GMLight.r_dsgraph_render_static(0, false);
								L->GMLight.r_dsgraph_render_dynamic(0, true);
								light_smap_keep::filled(*keep, L);
							}
							else
								L->GMLight.RGraph.clear_dynamic<false>(); // its dynamic casters are in the kept map
							L->m_smap_keep_srv = keep->srv;
							L->X.S.transluent = FALSE;
							continue;
						}
#endif
						Target->phase_smap_spot(L);
						RCache.set_xform_world(Fidentity);
						RCache.set_xform_view(L->X.S.view);
						RCache.set_xform_project(L->X.S.project);
						L->GMLight.r_dsgraph_render_static(0, false);
						L->GMLight.r_dsgraph_render_dynamic(0, true);
						if (Details && Details->dtFS && check_grass_shadow(L, ViewBase) && L->flags.bShadow && !decorative_light)
						{
							Details->fade_distance = -1; // Use light position to calc "fade"
							Details->light_position.set(L->position);
							Details->hw_Render(L);
						}
					
						L->X.S.transluent = FALSE;
						if (bForward_Shadows)
						{
							L->X.S.transluent = TRUE;
							Target->phase_smap_spot_tsh(L);
					
							L->GMLight.r_dsgraph_render_static(1, false);
							L->GMLight.r_dsgraph_render_dynamic(1, true);
					
							L->GMLight.r_dsgraph_render_sorted();			// strict-sorted geoms
						}
					}
					else if (L->flags.bVolumetric && ps_r2_ls_flags.test(R2FLAG_VOLUMETRIC_LIGHTS))
					{
						L_spot_s.push_back(L);
					}
				}
			}
			//		if (was_spot_shadowed)		->	accum spot shadowed
			if (!L_spot_s.empty())
			{
				PROF_EVENT("ACCUM_SPOT");
				stats.ls_shadowed_rendered += (u32)L_spot_s.size();
				for (light* L : L_spot_s)
				{
#ifdef USE_DX11
					Target->smap_keep_bind(L);
#endif
#ifdef USE_DX12
					vrs::begin();
#endif
					Target->accum_spot(L);
#ifdef USE_DX12
					vrs::end();
#endif
					render_indirect(L);
					if (L->flags.bVolumetric && RImplementation.o.advancedpp && ps_r2_ls_flags.is(R2FLAG_VOLUMETRIC_LIGHTS))
					{
#ifdef USE_DX11
						float w = float(Device.dwRenderWidth);
						float h = float(Device.dwRenderHeight);

						if (RImplementation.o.ssfx_volumetric)
							Target->set_viewport_size(HW.pContext, w / RImplementation.o.volsize, h / RImplementation.o.volsize);
#endif

						if (ps_pfx_volumetric_mode == 1)
							Target->accum_volumetric_lv(L);
						else
							Target->accum_volumetric(L);

#ifdef USE_DX11
						// Restore resolution
						if (RImplementation.o.ssfx_volumetric)
							Target->set_viewport_size(HW.pContext, w, h);
#endif
					}
#ifdef USE_DX11
					Target->smap_keep_unbind(L);
#endif
				}

				L_spot_s.clear();
			}
		}
	}

	{
#if defined(USE_DX10) || defined(USE_DX11)
		PIX_EVENT(UNSHADOWED_LIGHTS);
#endif
        PROF_EVENT("UNSHADOWED_LIGHTS");
		{
#if defined(USE_DX10) || defined(USE_DX11)
			PIX_EVENT(POINT_LIGHTS_ACCUM_UNSH);
#endif
			// Point lighting (unshadowed, if left)
			if (!LP.v_point.empty())
			{
				for (light* L : LP.v_point)
				{
					L->vis_update();
					if (!L->vis.visible)
						continue;

#ifdef USE_DX12
					vrs::begin();
#endif
					Target->accum_point(L);
#ifdef USE_DX12
					vrs::end();
#endif
					++stats.ls_unshadowed_point_rendered;
					render_indirect(L);
				}
				LP.v_point.clear();
			}
		}
		{
#if defined(USE_DX10) || defined(USE_DX11)
			PIX_EVENT(SPOT_LIGHTS_ACCUM_UNSH);
#endif
			// Spot lighting (unshadowed, if left)
			if (!LP.v_spot.empty())
			{
				for (light* L : LP.v_spot)
				{
					L->vis_update();
					if (!L->vis.visible)
						continue;

#ifdef USE_DX12
					vrs::begin();
#endif
					Target->accum_spot(L);
#ifdef USE_DX12
					vrs::end();
#endif
					++stats.ls_unshadowed_spot_rendered;
					render_indirect(L);
				}
				LP.v_spot.clear();
			}
		}
	}


}

void CRender::render_indirect(light* L)
{
	if (!ps_r2_ls_flags.test(R2FLAG_GI)) return;

	light LIGEN;
	LIGEN.set_type(IRender_Light::REFLECTED);
	LIGEN.set_shadow(false);
	LIGEN.set_cone(PI_DIV_2 * 2.f);

	xr_vector<light_indirect>& Lvec = L->indirect;
	if (Lvec.empty()) return;
	float LE = L->color.intensity();
	for (u32 it = 0; it < Lvec.size(); it++)
	{
		light_indirect& LI = Lvec[it];

		// energy and color
		float LIE = LE * LI.E;
		if (LIE < ps_r2_GI_clip) continue;
		Fvector T;
		T.set(L->color.r, L->color.g, L->color.b).mul(LI.E);
		LIGEN.set_color(T.x, T.y, T.z);

		// geometric
		Fvector L_up, L_right;
		L_up.set(0, 1, 0);
		if (_abs(L_up.dotproduct(LI.D)) > .99f) L_up.set(0, 0, 1);
		L_right.crossproduct(L_up, LI.D).normalize();
		LIGEN.SpatialComponent->spatial.sector = LI.S;
		LIGEN.set_position(LI.P);
		LIGEN.set_rotation(LI.D, L_right);

		// range
		// dist^2 / range^2 = A - has infinity number of solutions
		// approximate energy by linear fallof Emax / (1 + x) = Emin
		float Emax = LIE;
		float Emin = 1.f / 255.f;
		float x = (Emax - Emin) / Emin;
		if (x < 0.1f) continue;
		LIGEN.set_range(x);

		Target->accum_reflected(&LIGEN);
	}
}
