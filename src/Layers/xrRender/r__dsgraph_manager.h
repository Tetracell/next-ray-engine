#if !defined(_DSGMANAGE_H_)
#define _DSGMANAGE_H_
#pragma once
#include "r__dsgraph_types.h"
#include "r__sector.h"
#include "../../xrCore/FixedMap.h"
class CDSGraphManager : public IDSGraphManager
{
public:
	enum
	{
		VQ_HOM = (1 << 0),
		VQ_SSA = (1 << 1),
		VQ_FADE = (1 << 2),				// requires SSA to work
	};

	R_dsgraph::DynamicSceneRgraph RGraph;
	xr_list<Fmatrix> owned_transforms;
	xr_vector<IRenderable*> renderable_ui;
	virtual void add_RenderableUI(IRenderable* object) override
	{
		if (i_mask[fl_normal]) renderable_ui.push_back(object);
	}
	void r_dsgraph_render_ui();
	void r_dsgraph_render_ui_3d();
	u32										i_options;		// input:	culling options
	u32										i_doptions;
	int										i_sun_cascade = -1;	// input:	sun cascade this graph captures casters for, -1 none
	int										i_static_filter = 0;	// input:	static objects inserted: 0 all, 1 all but trees and bushes, 2 only those (r4_sun_cache.h)
	u32										i_static_trees = 0;	// output:	trees and bushes among the static objects inserted (light_smap_keep)
	Fvector									i_vBase;		// input:	"view" point
	CFrustum								i_frustum;		// input:	"view" frustum
	Fmatrix									i_mXFORM;		// input:	4x4 xform
	CSector* i_start;		// input:	starting point
	xrCriticalSection						P_CS;
	xrSRWLock								S_LC;

	FixedMAP<CSector*, std::pair<xr_vector<CFrustum>, FixedSet<CPortal*>>>		m_sector_frustums;
	xr_unordered_flat_set<dxRender_Visual*>				m_static_seen;
	xr_vector<ISpatialShared>				lstRenderables, lstLights;
	FixedMAP<CPortal*, float>				f_portals;
	sPoly S, D;
	ref_shader								f_shader;
	ref_geom								f_geom;

	CDSGraphManager(u32 options, u32 doptions, bool(&& mask)[7]) : i_options(options), i_doptions(doptions)
	{
		std::copy(mask, mask + 7, i_mask);
	}
	void initialize();
	void destroy();
	void traverse(CSector* start, CFrustum& F, Fvector& vBase, Fmatrix& mXFORM);
	IC bool is_sector_visible(CSector* sector)
	{
		if (sector == i_start) return true;

		{
			xrSRWLockGuard guard(&S_LC, true);
			if(m_sector_frustums.size())
			{
				if (auto node = m_sector_frustums.find(sector))
					return !node->val.first.empty();
			}
		}

		return false;
	};
	CDSGraphManager& get_traverser_safed() { xrSRWLockGuard guard(&S_LC, false); return *this; };
	void fade_portal(CPortal* _p, float ssa);
	void fade_render();

	virtual void set_Object(IRenderable* O = nullptr);

	virtual void add_Static(IRenderVisual* pVisual, CFrustum& frustum, u32 planes);
	virtual void add_Static_MultiFrustum(IRenderVisual* pVisual, const xr_vector<CFrustum>& frustums, const xr_vector<u32>& masks);
	void add_leaf_Static(dxRender_Visual* pVisual);
	void add_leafs_Static(xr_vector<dxRender_Visual*>& children);
	void add_leafs_Static(xr_vector<IRenderVisual*>& children);
	void r_dsgraph_insert_static(dxRender_Visual* pVisual, bool prefiltered = false);
#ifdef USE_DX11
	// Main view static geometry in parallel (r4_main_parallel_cull): the visibility tests on worker threads, then the
	// visible visuals inserted on this thread in the order add_Static would have inserted them
	struct static_item
	{
		dxRender_Visual* visual;
		float D, ssa;
		bool lod; // a LOD's far version (mapLOD), else a visual for r_dsgraph_insert_static
	};
	bool add_Static_parallel(IRenderVisual* root, CFrustum& frustum, u32 planes);
	void collect_Static(dxRender_Visual* pVisual, const CFrustum& frustum, u32 planes, xr_vector<static_item>& out, CRandom& rnd);
	void collect_leaf_Static(dxRender_Visual* pVisual, xr_vector<static_item>& out, CRandom& rnd);
	bool prefilter_static(dxRender_Visual* pVisual);
#endif

	virtual void add_Dynamic(IRenderVisual* piVisual, Fmatrix* xform);
	void add_Dynamic(dxRender_Visual* pVisual, Fmatrix* xform);
	void add_leafs_Dynamic(xr_vector<dxRender_Visual*>& children, Fmatrix* xform);
	void add_leafs_Dynamic(xr_vector<IRenderVisual*>& children, Fmatrix* xform);
	void r_dsgraph_insert_dynamic(dxRender_Visual* pVisual, Fmatrix* xform);

	void AddToRenderQueue(R_dsgraph::RenderQueue& queue, const R_dsgraph::DSGraphItem<u32, false>& item, const SPass& pass);
	void r_dsgraph_render_graph(R_dsgraph::RenderQueueArray& queue, u32 _priority, bool _clear = true, bool static_geometry = true);
	IC void r_dsgraph_render_graph(u32 _priority, bool _clear = true)
	{
		r_dsgraph_render_static(_priority, _clear);
		r_dsgraph_render_dynamic(_priority, _clear);
	}
	IC void r_dsgraph_render_static(u32 _priority, bool _clear = true)
	{
		PROF_EVENT("r_dsgraph_render_static");
		r_dsgraph_render_graph(RGraph.mapStaticPasses, _priority, _clear);
	};
	IC void	r_dsgraph_render_dynamic(u32 _priority, bool _clear = true)
	{
		PROF_EVENT("r_dsgraph_render_dynamic");
		r_dsgraph_render_graph(RGraph.mapDynamicPasses, _priority, _clear, false);
	};

	template<typename T, bool Reverse>
	void r_dsgraph_render_graph_sorted(R_dsgraph::mapDSGraphItems<T, Reverse>& graph, bool _clear = true);
	void r_dsgraph_capture_hud();
	void r_dsgraph_render_hud();
	void r_dsgraph_render_hud_ui();
	void r_dsgraph_render_lods(bool _setup_zb, bool _clear);
	void r_dsgraph_render_sorted(bool render_hud = true);
	void r_dsgraph_render_sorted_hud();
	void r_dsgraph_render_emissive(bool clear = true, bool renderHUD = false);
	void r_dsgraph_render_wmarks();
	void r_dsgraph_render_distort();

	// Anomaly
#if defined(USE_DX11)
	void r_dsgraph_render_ScopeSorted();
	void r_dsgraph_render_PDALate();
#endif
	void r_dsgraph_render_cam_ui();
	void r_dsgraph_render_water_ssr();
	void r_dsgraph_render_water();

	void r_dsgraph_capture(bool lights = false, bool dynamic = false, CObject* O = nullptr);
	void r_dsgraph_capture_lights();
	void r_dsgraph_capture_static();
	void r_dsgraph_capture_dynamic(CObject* O = nullptr);
	// r_dsgraph_capture_dynamic's spatial query run ahead of it: it only reads the spatial database (shared lock) and
	// fills this graph's own list, so the lamps' queries run in parallel (r4_lights_parallel_query)
	void r_dsgraph_prefetch_dynamic();
	bool m_dynamic_prefetched = false;

	void clear()
	{
		xrSRWLockGuard guard(&S_LC, false);
		RGraph.clear();
		renderable_ui.clear();
		owned_transforms.clear();
		if (m_sector_frustums.size())
		{
			for (auto& pair : m_sector_frustums)
			{
				pair.val.first.clear();
				pair.val.second.clear();
			}
			m_sector_frustums.clear();
		}
		lstRenderables.clear();
		lstLights.clear();
		f_portals.clear();
		m_static_seen.clear();
	}
};

struct PortalTraverseDebugStats
{
	u32 frame_id = 0;

	u32 traverse_calls = 0;
	u32 traverse_calls_with_options = 0;
	u32 traverse_calls_without_options = 0;
	u32 frustums_pushed = 0;
	u32 max_frustums_in_sector = 0;
	u32 frustums_pushed_opt = 0;
	u32 frustums_pushed_noopt = 0;
	u32 max_frustums_in_sector_opt = 0;
	u32 max_frustums_in_sector_noopt = 0;

	u32 portals_checked = 0;
	u32 portals_checked_opt = 0;
	u32 portals_checked_noopt = 0;
	u32 portals_skipped_already_visited = 0;
	u32 portals_rejected_sphere = 0;
	u32 portals_rejected_sector = 0;
	u32 portals_rejected_ssa = 0;
	u32 portals_rejected_clip = 0;
	u32 portals_rejected_hom = 0;
	u32 portals_recursed = 0;
	u32 portals_recursed_opt = 0;
	u32 portals_recursed_noopt = 0;

	u32 portals_debug_branch_taken = 0;
	u32 portals_debug_branch_precedence_hits = 0;

	u32 static_sector_nodes = 0;
	u32 static_frustum_nodes = 0;
	u32 static_add_root_calls = 0;
	u32 static_sector_nodes_opt = 0;
	u32 static_sector_nodes_noopt = 0;
	u32 static_frustum_nodes_opt = 0;
	u32 static_frustum_nodes_noopt = 0;
	u32 static_add_root_calls_opt = 0;
	u32 static_add_root_calls_noopt = 0;

	u32 dynamic_spatials = 0;
	u32 dynamic_frustum_tests = 0;
	u32 dynamic_frustum_hits = 0;
	u32 dynamic_rendered = 0;
	u32 dynamic_spatials_opt = 0;
	u32 dynamic_spatials_noopt = 0;
	u32 dynamic_frustum_tests_opt = 0;
	u32 dynamic_frustum_tests_noopt = 0;
	u32 dynamic_frustum_hits_opt = 0;
	u32 dynamic_frustum_hits_noopt = 0;
	u32 dynamic_rendered_opt = 0;
	u32 dynamic_rendered_noopt = 0;

	u32 queue_static_packets = 0;
	u32 queue_dynamic_packets = 0;
	u32 queue_static_packets_opt = 0;
	u32 queue_static_packets_noopt = 0;
	u32 queue_dynamic_packets_opt = 0;
	u32 queue_dynamic_packets_noopt = 0;

	u32 static_dedup_seen = 0;
	u32 static_dedup_skipped = 0;
	u32 static_dedup_seen_opt = 0;
	u32 static_dedup_seen_noopt = 0;
	u32 static_dedup_skipped_opt = 0;
	u32 static_dedup_skipped_noopt = 0;

	void reset(u32 frame)
	{
		*this = {};
		frame_id = frame;
	}
};

bool PortalTraverseDbg_Enabled();
bool PortalTraverseDbg_IsOptions(u32 options);
PortalTraverseDebugStats& PortalTraverseDbg_Get();
const PortalTraverseDebugStats& PortalTraverseDbg_Peek();

#endif // !defined(AFX_PORTAL_H__1FC2D371_4A19_49EA_BD1E_2D0F8DEBBF15__INCLUDED_)
