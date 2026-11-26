#include "stdafx.h"
#include "EngineThreading.h"
#include "CustomHUD.h"
#include "IGame_Persistent.h"
#include "IGame_Level.h"
#include "Rain.h"
#include "../../xrCDB/ISpatial.h"
#include "../../xrCDB/Frustum.h"
#include "Render.h"
#include "Irenderable.h"
#include "../../Include/xrRender/Kinematics.h"

BOOL mt_Scheduler = TRUE;
extern BOOL mt_calc_bones_parallel;
BOOL mt_SchedulerRT = FALSE;

void XRay::Engine::PreRenderThread()
{
	PROF_THREAD("Secondary Task 1");
	mt_timed t(mt_t_rain_particles);

	if (g_pGamePersistent && g_pGamePersistent->pEnvironment && g_pGamePersistent->pEnvironment->eff_Rain)
	{
		PROF_EVENT("CEffect_Rain::UpdateItems");
		g_pGamePersistent->pEnvironment->eff_Rain->UpdateItems();
	}

	if (g_pGamePersistent && Device.ParticleWorkerCallback)
	{
		PROF_EVENT("Process Particles");
		Device.ParticleWorkerCallback();
	}
}

void XRay::Engine::PreRenderPostTransformsThread()
{
    PROF_THREAD("Secondary Task 1.1");
    mt_timed t(mt_t_parallel_render);
    {
        PROF_EVENT("seqParallelRender");
        for (auto& it : Device.seqParallelRender)
            it();
    }
}

struct SpatialSnapshot
{
	ISpatialShared ptr;
    IKinematics* pKin;
    float distSq;

    SpatialSnapshot(ISpatialShared _ptr, IKinematics* _pKin, float _distSq) : ptr(_ptr), pKin(_pKin), distSq(_distSq) {};
};
void XRay::Engine::CalculateBonesThread()
{
	PROF_THREAD("Secondary Task 3");

	PROF_EVENT("CalculateBones");
	mt_timed t(mt_t_bones);

	if (!g_SpatialSpace) return;
	if (Device.Paused()) return;
	if (!psDeviceFlags.test(rsDrawDynamic)) return;
	if (!g_pGameLevel || !g_pGameLevel->bReady) return;
	if (!g_pGameLevel->CurrentEntity()) return;

	static CFrustum ViewBase;
	ViewBase.CreateFromMatrix(Device.mFullTransform_saved, FRUSTUM_P_LRTB | FRUSTUM_P_FAR);

	static xr_vector<ISpatialShared> spatials = {};
    spatials.clear();
	g_SpatialSpace->q_sphere(
		spatials,
		ISpatial_DB::O_ORDERED,
		STYPE_RENDERABLE + STYPE_RENDERABLESHADOW + STYPE_PARTICLE + STYPE_LIGHTSOURCE,
		Device.vCameraPosition_saved,
		g_pGamePersistent->Environment().CurrentEnv->fog_distance
	);

	static xr_vector<SpatialSnapshot> spatialsSnapshot;
	spatialsSnapshot.clear();
	{
		for (ISpatialShared spatial : spatials)
		{
			if (!spatial)
				continue;

            float distSq = Device.vCameraPosition_saved.distance_to_sqr(spatial->spatial.sphere.P);
			if (!ViewBase.testSphere_dirty(spatial->spatial.sphere.P, spatial->spatial.sphere.R))
			{
				if (distSq > 62500.f)//250 m
					continue;
			}
				
			spatial->spatial_updatesector();

            if
            (
                (spatial->spatial.type & (STYPE_PARTICLE | STYPE_LIGHTSOURCE)) ||
                !(spatial->spatial.type & (STYPE_RENDERABLE | STYPE_RENDERABLESHADOW))
            )
                continue;

            auto renderable = spatial->dcast_Renderable();
            if (!(renderable && renderable->renderable.visual))
                continue;

            IKinematics* pKin = renderable->renderable.visual->dcast_PKinematics();
            if (!pKin)
                continue;

			spatialsSnapshot.emplace_back(spatial, pKin, distSq);
		}
	}

	static auto sortFunc = [](const SpatialSnapshot& _1, const SpatialSnapshot& _2) noexcept
	{
		return _1.distSq < _2.distSq;
	};
	std::sort(spatialsSnapshot.begin(), spatialsSnapshot.end(), sortFunc);

	if (!mt_calc_bones_parallel || spatialsSnapshot.size() < 4)
	{
		for (const auto& snapshot : spatialsSnapshot)
			snapshot.pKin->CalculateBones(TRUE);
		return;
	}

	// The animation tracks first, one model after another as before: their end callbacks run game code, some of it
	// Lua (script-controlled creatures), which must not run on two threads at once
	{
		PROF_EVENT("CalculateBones: tracks");
		for (const auto& snapshot : spatialsSnapshot)
			snapshot.pKin->CalculateTracks();
	}
	// Then the bones of the models on all cores. A model's bones are its own state, calculated under its own lock;
	// the bone callbacks (head and aim turning, ragdolls, attachments) read their own object only. The update
	// callbacks are held back: foot IK ray-picks other models, which takes their locks (two models picking each other
	// on two threads froze the game), so they run afterwards one model after another, as before
	{
		PROF_EVENT("CalculateBones: bones");
		xr_parallel_for(0u, u32(spatialsSnapshot.size()), [](u32 i) { spatialsSnapshot[i].pKin->CalculateBones_HoldCallback(TRUE); });
	}
	{
		PROF_EVENT("CalculateBones: callbacks");
		for (const auto& snapshot : spatialsSnapshot)
			snapshot.pKin->CalculateBones_RunCallback();
	}
}

extern BOOL psLua_ParallelGC;
int psLua_ParallelGC_CallAmount = 25;
void XRay::Engine::GameThread()
{
	PROF_THREAD("Secondary Task 2")
		
	// we has granted permission to execute
	if (g_hud)
	{
		PROF_EVENT("g_hud OnFrameMT");
		mt_timed t(mt_t_hud);
		g_hud->OnFrameMT();
	}

	if (g_pGameLevel && g_pGameLevel->bReady)
	{
		PROF_EVENT("SoundEvent_Dispatch");
		mt_timed t(mt_t_sound);
		g_pGameLevel->SoundEvent_Dispatch();
	}

	if (!Device.Paused())
	{
		if (mt_Scheduler)
		{
			PROF_EVENT("Sheduler Deferred");
			mt_timed t(mt_t_scheduler);
            if (mt_SchedulerRT)
            {
                ::Engine.Sheduler.Update();
            }
            else
            {
                ::Engine.Sheduler.UpdateDeferred();
                ::Engine.Sheduler.UpdateFinalize();
            }
		}
	}

	{
		PROF_EVENT("seqParallel");
		mt_timed t(mt_t_parallel);
		for (u32 pit = 0; pit < Device.seqParallel.size(); pit++)
			Device.seqParallel[pit]();
		Device.seqParallel.clear();
	}

	// Physics contact callbacks in seqFrameMT can enter Lua. Do before GC
	{
		PROF_EVENT("seqFrameMT");
		mt_timed t(mt_t_frame_mt);
		Device.seqFrameMT.Process(rp_Frame);
	}

    // demonized: While Renderer prepares frame and GPU renders it, use time opportunity to repeatedly call Lua GC with small step value
    // Reduces stutters since less work will be done in main GC step or no work at all
    static auto LuaGC = []()
    {
        PROF_EVENT("seqLuaGC");
        mt_timed t(mt_t_lua_gc);
        // Do at least once
        do
        {
            Device.LuaGCCount++;
            if (Device.LuaGC() == 1) // 1 informs that GC cycle is complete
            {
                Device.LuaGCDone = true;
                break;
            }

        } while (Device.isRendering && Device.LuaGCCount < psLua_ParallelGC_CallAmount);
    };
    if (psLua_ParallelGC && Device.LuaGC)
        Device.secondary_tasks.run(LuaGC);
}
