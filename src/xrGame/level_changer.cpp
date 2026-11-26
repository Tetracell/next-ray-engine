////////////////////////////////////////////////////////////////////////////
//	Module 		: level_changer.cpp
//	Created 	: 10.07.2003
//  Modified 	: 10.07.2003
//	Author		: Dmitriy Iassenev
//	Description : Level change object
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "level_changer.h"
#include "../xrCore/level_preload.h"
#include "../xrEngine/x_ray.h"
#include "hit.h"
#include "actor.h"
#include "xrserver_objects_alife.h"
#include "level.h"
#include "ai_object_location.h"
#include "ai_space.h"
#include "level_graph.h"
#include "game_level_cross_table.h"

#include "UIGameSP.h"
#include "../xrengine/xr_collide_form.h"

xr_vector<CLevelChanger*> g_lchangers;

CLevelChanger::~CLevelChanger()
{
}

void CLevelChanger::Center(Fvector& C) const
{
	XFORM().transform_tiny(C, CFORM()->getSphere().P);
}

float CLevelChanger::Radius() const
{
	return CFORM()->getRadius();
}

void CLevelChanger::net_Destroy()
{
	inherited::net_Destroy();
	xr_vector<CLevelChanger*>::iterator it = std::find(g_lchangers.begin(), g_lchangers.end(), this);
	if (it != g_lchangers.end())
		g_lchangers.erase(it);
}

#define DEF_INVITATION "level_changer_invitation"

BOOL CLevelChanger::net_Spawn(CSE_Abstract* DC)
{
	m_entrance_time = 0;
	m_b_enabled = true;
	m_invite_str = DEF_INVITATION;
	CCF_Shape* l_pShape = xr_new<CCF_Shape>(this);
	collidable.model = l_pShape;

	CSE_Abstract* l_tpAbstract = (CSE_Abstract*)(DC);
	CSE_ALifeLevelChanger* l_tpALifeLevelChanger = smart_cast<CSE_ALifeLevelChanger*>(l_tpAbstract);
	R_ASSERT(l_tpALifeLevelChanger);

	m_game_vertex_id = l_tpALifeLevelChanger->m_tNextGraphID;
	m_level_vertex_id = l_tpALifeLevelChanger->m_dwNextNodeID;
	m_position = l_tpALifeLevelChanger->m_tNextPosition;
	m_angles = l_tpALifeLevelChanger->m_tAngles;

	m_bSilentMode = !!l_tpALifeLevelChanger->m_bSilentMode;
	if (ai().get_level_graph())
	{
		//. this information should be computed in xrAI
		ai_location().level_vertex(ai().level_graph().vertex(u32(-1), Position()));
		ai_location().game_vertex(ai().cross_table().vertex(ai_location().level_vertex_id()).game_vertex_id());
	}

	feel_touch.clear();

	for (u32 i = 0; i < l_tpALifeLevelChanger->shapes.size(); ++i)
	{
		CSE_Shape::shape_def& S = l_tpALifeLevelChanger->shapes[i];
		switch (S.type)
		{
		case 0:
			{
				l_pShape->add_sphere(S.data.sphere);
				break;
			}
		case 1:
			{
				l_pShape->add_box(S.data.box);
				break;
			}
		}
	}

	BOOL bOk = inherited::net_Spawn(DC);
	if (bOk)
	{
		l_pShape->ComputeBounds();
		Fvector P;
		XFORM().transform_tiny(P, CFORM()->getSphere().P);
		setEnabled(TRUE);
	}
	g_lchangers.push_back(this);
	return (bOk);
}

void CLevelChanger::shedule_Update(u32 dt)
{
	inherited::shedule_Update(dt);

	const Fsphere& s = CFORM()->getSphere();
	Fvector P;
	XFORM().transform_tiny(P, s.P);
	feel_touch_update(P, s.R);

	update_actor_invitation();

	// Its level read ahead while the actor is within 80 m (level_preload.h)
	CActor* actor = Actor();
	if (level_preload::enabled && m_b_enabled && actor && actor->g_Alive() && actor->Position().distance_to_sqr(P) < 80.f * 80.f)
		level_preload::start(target_level());
}

LPCSTR CLevelChanger::target_level() const
{
	if (!ai().get_game_graph() || !ai().game_graph().valid_vertex_id(m_game_vertex_id))
		return nullptr;
	return *ai().game_graph().header().level(ai().game_graph().vertex(m_game_vertex_id)->level_id()).name();
}

#include "patrol_path.h"
#include "patrol_path_storage.h"

static void send_change_level_now(GameGraph::_GRAPH_ID game_vertex_id, u32 level_vertex_id, const Fvector& position,
                                  const Fvector& angles)
{
	NET_Packet p;
	p.w_begin(M_CHANGE_LEVEL);
	p.w(&game_vertex_id, sizeof(game_vertex_id));
	p.w(&level_vertex_id, sizeof(level_vertex_id));
	p.w_vec3(position);
	p.w_vec3(angles);
	Level().Send(p, net_flags(TRUE));
}

void send_change_level(GameGraph::_GRAPH_ID game_vertex_id, u32 level_vertex_id, const Fvector& position,
                       const Fvector& angles)
{
	if (!g_fade_transitions)
	{
		send_change_level_now(game_vertex_id, level_vertex_id, position, angles);
		return;
	}
	// Already fading out to travel: one request
	if (g_screen_fade.busy())
		return;
	IGame_Level* level = g_pGameLevel;
	g_screen_fade.fade_out(400, [=]()
	{
		// Still the level that asked (no saved game loaded meanwhile)
		if (g_pGameLevel == level && g_pGameLevel)
			send_change_level_now(game_vertex_id, level_vertex_id, position, angles);
	});
}

// rc_silent_level_changers: every level changer travels at once, without asking (automated runs: the remote
// console can't answer the dialog). Never saved
int g_rc_silent_level_changers = 0;

void CLevelChanger::feel_touch_new(CObject* tpObject)
{
	CActor* l_tpActor = smart_cast<CActor*>(tpObject);
	VERIFY(l_tpActor);
	if (!l_tpActor->g_Alive())
		return;

	if (m_bSilentMode || g_rc_silent_level_changers)
	{
		send_change_level(m_game_vertex_id, m_level_vertex_id, m_position, m_angles);
		return;
	}
	Fvector p, r;
	bool b = get_reject_pos(p, r);
	CUIGameSP* pGameSP = smart_cast<CUIGameSP*>(CurrentGameUI());
	if (pGameSP)
		pGameSP->ChangeLevel(m_game_vertex_id, m_level_vertex_id, m_position, m_angles, p, r, b, m_invite_str,
		                     m_b_enabled);

	m_entrance_time = Device.fTimeGlobal;
}

bool CLevelChanger::get_reject_pos(Fvector& p, Fvector& r)
{
	p.set(0, 0, 0);
	r.set(0, 0, 0);
	//--		db.actor:set_actor_position(patrol("t_way"):point(0))
	//--		local dir = patrol("t_look"):point(0):sub(patrol("t_way"):point(0))
	//--		db.actor:set_actor_direction(-dir:getH())

	if (m_ini_file && m_ini_file->section_exist("pt_move_if_reject"))
	{
		LPCSTR p_name = m_ini_file->r_string("pt_move_if_reject", "path");
		const CPatrolPath* patrol_path = ai().patrol_paths().safe_path(p_name, false, true);
		VERIFY(patrol_path);

		const CPatrolPoint* pt;
		pt = &patrol_path->vertex(0)->data();
		p = pt->position();

		Fvector tmp;
		pt = &patrol_path->vertex(1)->data();
		tmp.sub(pt->position(), p);
		tmp.getHP(r.y, r.x);
		return true;
	}
	return false;
}

bool CLevelChanger::feel_touch_contact(CObject* object)
{
	bool bRes = ((((CCF_Shape*)CFORM())->Contact(object)) != 0);
	bRes = bRes && smart_cast<CActor*>(object) && smart_cast<CActor*>(object)->g_Alive();
	return bRes;
}

void CLevelChanger::update_actor_invitation()
{
	if (m_bSilentMode || g_rc_silent_level_changers) return;
	xr_vector<CObject*>::iterator it = feel_touch.begin();
	xr_vector<CObject*>::iterator it_e = feel_touch.end();

	for (; it != it_e; ++it)
	{
		CActor* l_tpActor = smart_cast<CActor*>(*it);
		VERIFY(l_tpActor);

		if (!l_tpActor->g_Alive())
			continue;

		if (m_entrance_time + 5.0f < Device.fTimeGlobal)
		{
			CUIGameSP* pGameSP = smart_cast<CUIGameSP*>(CurrentGameUI());
			Fvector p, r;
			bool b = get_reject_pos(p, r);

			if (pGameSP)
				pGameSP->ChangeLevel(m_game_vertex_id, m_level_vertex_id, m_position, m_angles, p, r, b, m_invite_str,
				                     m_b_enabled);

			m_entrance_time = Device.fTimeGlobal;
		}
	}
}

void CLevelChanger::save(NET_Packet& output_packet)
{
	inherited::save(output_packet);
	output_packet.w_stringZ(m_invite_str);
	output_packet.w_u8(m_b_enabled ? 1 : 0);
}

void CLevelChanger::load(IReader& input_packet)
{
	inherited::load(input_packet);
	input_packet.r_stringZ(m_invite_str);
	m_b_enabled = !!input_packet.r_u8();
}

BOOL CLevelChanger::net_SaveRelevant()
{
	if (!m_b_enabled || m_invite_str != DEF_INVITATION)
		return TRUE;
	else
		return inherited::net_SaveRelevant();
}
