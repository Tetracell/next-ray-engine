#include "stdafx.h"
#pragma hdrstop

#pragma warning(disable:4995)
#include <d3dx9.h>
#ifndef _EDITOR
#pragma comment( lib, "d3dx9.lib"		)
#include "../../xrEngine/render.h"
#endif
#pragma warning(default:4995)

#include <D3DX10Core.h>

#include "../xrRender/ResourceManager.h"
#include "../xrRender/shader_patches.h"
#include "../xrRender/tss.h"
#include "../xrRender/blenders/blender.h"
#include "../xrRender/blenders/blender_recorder.h"

#include "../xrRenderDX10/dx10BufferUtils.h"
#include "../xrRenderDX10/dx10ConstantBuffer.h"

#include "../xrRender/ShaderResourceTraits.h"

#ifdef USE_DX11
SHS* CResourceManager::_CreateHS(LPCSTR Name)
{
	return CreateShader<SHS>(Name);
}

void CResourceManager::_DeleteHS(const SHS* HS)
{
	DestroyShader(HS);
}

SDS* CResourceManager::_CreateDS(LPCSTR Name)
{
	return CreateShader<SDS>(Name);
}

void CResourceManager::_DeleteDS(const SDS* DS)
{
	DestroyShader(DS);
}

SCS* CResourceManager::_CreateCS(LPCSTR Name)
{
	return CreateShader<SCS>(Name);
}

void CResourceManager::_DeleteCS(const SCS* CS)
{
	DestroyShader(CS);
}
#endif	//	USE_DX10

void fix_texture_name(LPSTR fn);

template <class T>
BOOL reclaim(xr_vector<T*>& vec, const T* ptr)
{
	xr_vector<T*>::iterator it = vec.begin();
	xr_vector<T*>::iterator end = vec.end();
	for (; it != end; it++)
		if (*it == ptr)
		{
			vec.erase(it);
			return TRUE;
		}
	return FALSE;
}

//--------------------------------------------------------------------------------------------------------------
SState* CResourceManager::_CreateState(SimulatorStates& state_code)
{
	xrCriticalSectionGuard guard(creationGuard);

	// Search equal state-code 
	for (u32 it = 0; it < v_states.size(); it++)
	{
		SState* C = v_states[it];;
		SimulatorStates& base = C->state_code;
		if (base.equal(state_code)) return C;
	}

	// Create New
	v_states.push_back(xr_new<SState>());
	v_states.back()->dwFlags |= xr_resource_flagged::RF_REGISTERED;
#if defined(USE_DX10) || defined(USE_DX11)
	v_states.back()->state = ID3DState::Create(state_code);
#else	//	USE_DX10
	v_states.back()->state			= state_code.record();
#endif	//	USE_DX10
	v_states.back()->state_code = state_code;
	return v_states.back();
}

void CResourceManager::_DeleteState(const SState* state)
{
	if (0 == (state->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(creationGuard);
	if (reclaim(v_states, state)) return;
	Msg("! ERROR: Failed to find compiled stateblock");
}

//--------------------------------------------------------------------------------------------------------------
SPass* CResourceManager::_CreatePass(const SPass& proto)
{
	xrCriticalSectionGuard guard(creationGuard);
	for (u32 it = 0; it < v_passes.size(); it++)
		if (v_passes[it]->equal(proto))
			return v_passes[it];

	SPass* P = xr_new<SPass>();
	P->dwFlags |= xr_resource_flagged::RF_REGISTERED;
	P->state = proto.state;
	P->ps = proto.ps;
	P->vs = proto.vs;
	P->gs = proto.gs;
#ifdef USE_DX11
	P->hs = proto.hs;
	P->ds = proto.ds;
	P->cs = proto.cs;
#endif
	P->constants = proto.constants;
	P->T = proto.T;
#ifdef _EDITOR
	P->M						=	proto.M;
#endif
	P->C = proto.C;

	v_passes.push_back(P);
	return v_passes.back();
}

void CResourceManager::_DeletePass(const SPass* P)
{
	if (0 == (P->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(creationGuard);
	if (reclaim(v_passes, P)) return;
	Msg("! ERROR: Failed to find compiled pass");
}

#if defined(USE_DX11)
// SSS's grass (deffer_grass.vs) takes the TAA jitter only below a height of 0.1 (if (H < 0.1f) O.hpos.xy =
// ssfx_taa_jitter(O.hpos)), so the blade above its base was never jittered. The upscalers take the jitter back out of
// every pixel: the blades shook by it and were never sampled between pixels, grass stayed at render resolution
// (pixelated, most with DLSS and lower internal resolutions; PC test) while the rest came out sharp. With
// r4_upscaler_grass_jitter the condition also passes while the upscaler jitters (nextray_upscale_jitter not zero), in
// memory when compiled; without the upscaler SSS TAA keeps its own. Patched shaders compile under their own name
static bool patch_grass_jitter_shader(LPCSTR data, xr_string& patched)
{
	extern int ps_r4_upscaler_grass_jitter;
	if (!ps_r4_upscaler_grass_jitter || strstr(data, "nextray_upscale_jitter"))
		return false;

	static const char condition[] = "if (H < 0.1f)";
	static const char jitter[] = "O.hpos.xy = ssfx_taa_jitter(O.hpos);";
	LPCSTR hit = strstr(data, condition);
	if (!hit)
		return false;
	LPCSTR next = hit + sizeof(condition) - 1;
	while (*next == ' ' || *next == '\t' || *next == '\r' || *next == '\n')
		++next;
	if (strncmp(next, jitter, sizeof(jitter) - 1) || strstr(next, condition))
		return false;

	// The declaration first, then the file as it is, its line numbers kept for compile errors
	patched = "uniform float4 nextray_upscale_jitter;\n#line 1\n";
	patched.append(data, hit - data);
	patched += "if (H < 0.1f || any(nextray_upscale_jitter.xy != 0))";
	patched += hit + sizeof(condition) - 1;
	return true;
}
#endif

#if defined(USE_DX11)
// Tree instancing (r4_tree_instancing, r__dsgraph_render.cpp): a tree vertex shader asked for as <name>_xrinst is the
// shader's own source with its per-tree constants (m_xform, m_xform_v, c_scale, c_bias, c_sun) made statics that main
// sets from xr_tree_inst for its instance (9 rows each, as FTreeVisual::write_instance fills them), in memory when
// compiled: the file is untouched. False when the source isn't one this knows (its trees are then drawn one by one)
static bool patch_tree_instancing_shader(LPCSTR data, xr_string& patched)
{
	static const char* const per_tree[] = { "m_xform", "m_xform_v", "c_scale", "c_bias", "c_sun" };
	static const int row_of[] = { 0, 3, 6, 7, 8 };
	auto ident = [](char c) { return isalnum(u8(c)) || c == '_'; };
	auto find_word = [&](const xr_string& text, LPCSTR word, size_t from) -> size_t
	{
		const size_t len = xr_strlen(word);
		for (size_t at = text.find(word, from); at != xr_string::npos; at = text.find(word, at + 1))
			if ((at == 0 || !ident(text[at - 1])) && (at + len >= text.size() || !ident(text[at + len])))
				return at;
		return xr_string::npos;
	};
	auto trim = [](xr_string v)
	{
		const size_t a = v.find_first_not_of(" \t\r\n"), b = v.find_last_not_of(" \t\r\n");
		return a == xr_string::npos ? xr_string() : v.substr(a, b - a + 1);
	};

	xr_string src = data, assigns;
	bool has_xform = false;
	// Each "uniform <type> a, b;" on one line declaring a per-tree constant: those become statics
	for (size_t at = 0; (at = find_word(src, "uniform", at)) != xr_string::npos;)
	{
		const size_t line_start = src.rfind('\n', at) == xr_string::npos ? 0 : src.rfind('\n', at) + 1;
		const size_t end = src.find(';', at), line_end = src.find('\n', at);
		if (src.substr(line_start, at - line_start).find("//") != xr_string::npos || end == xr_string::npos ||
			(line_end != xr_string::npos && line_end < end))
		{
			at += 7;
			continue;
		}
		const xr_string decl = trim(src.substr(at + 7, end - at - 7));
		const size_t type_end = decl.find_first_of(" \t");
		if (type_end == xr_string::npos)
		{
			at = end;
			continue;
		}
		const xr_string type = decl.substr(0, type_end);
		xr_string kept, inst;
		for (size_t p = type_end; p < decl.size();)
		{
			size_t comma = decl.find(',', p);
			if (comma == xr_string::npos)
				comma = decl.size();
			const xr_string name = trim(decl.substr(p, comma - p));
			p = comma + 1;
			int which = -1;
			for (int i = 0; i < 5; ++i)
				if (name == per_tree[i])
					which = i;
			if (which < 0)
			{
				kept += (kept.empty() ? "" : ", ") + name;
				continue;
			}
			string256 row;
			xr_sprintf(row, "xr_tree_inst[xr_tree_iid * 9 + %d]", row_of[which]);
			string512 code;
			if (type == "float3x4")
			{
				xr_sprintf(code, " %s = float3x4(xr_tree_inst[xr_tree_iid * 9 + %d], xr_tree_inst[xr_tree_iid * 9 + %d], "
					"xr_tree_inst[xr_tree_iid * 9 + %d]);", name.c_str(), row_of[which], row_of[which] + 1, row_of[which] + 2);
				has_xform = has_xform || which == 0;
			}
			else if (type == "float4")
				xr_sprintf(code, " %s = %s;", name.c_str(), row);
			else if (type == "float3")
				xr_sprintf(code, " %s = %s.xyz;", name.c_str(), row);
			else if (type == "float2")
				xr_sprintf(code, " %s = %s.xy;", name.c_str(), row);
			else
				return false;
			assigns += code;
			inst += (inst.empty() ? "" : ", ") + name;
		}
		if (inst.empty())
		{
			at = end;
			continue;
		}
		const xr_string replacement = (kept.empty() ? xr_string() : "uniform " + type + " " + kept + "; ") + "static " + type + " " + inst;
		src.replace(at, end - at, replacement);
		at += replacement.size();
	}
	if (!has_xform)
		return false;

	// main gets the instance's index, and sets the per-tree values first. Every signature of main before its body gets
	// the index: a shader can declare main under #ifdef / #else over one body (SSS's shadow_direct_tree.vs)
	xr_vector<std::pair<size_t, size_t>> signatures; // the ( and ) of each
	size_t brace = xr_string::npos;
	for (size_t m = 0; (m = find_word(src, "main", m)) != xr_string::npos; m += 4)
	{
		if (brace != xr_string::npos && m > brace)
			break;
		const size_t open = src.find_first_not_of(" \t\r\n", m + 4);
		if (open == xr_string::npos || src[open] != '(')
			continue;
		const size_t close = src.find(')', open);
		if (close == xr_string::npos)
			return false;
		signatures.push_back({ open, close });
		if (brace == xr_string::npos && (brace = src.find('{', close)) == xr_string::npos)
			return false;
	}
	if (signatures.empty())
		return false;
	src.insert(brace + 1, assigns);
	for (auto it = signatures.rbegin(); it != signatures.rend(); ++it)
	{
		const bool empty = trim(src.substr(it->first + 1, it->second - it->first - 1)).empty();
		src.insert(it->second, empty ? "uint xr_tree_iid : SV_InstanceID" : ", uint xr_tree_iid : SV_InstanceID");
	}
	patched = "cbuffer xr_tree_instances { float4 xr_tree_inst[32 * 9]; }\n#line 1\n" + src;
	return true;
}
#endif

//--------------------------------------------------------------------------------------------------------------
SVS* CResourceManager::_CreateVS(LPCSTR _name)
{
	PROF_EVENT("_CreateVS");
	xrCriticalSectionGuard guard(creationGuard);
	xr_string res_name = _name;

	const int m_skinning = Engine.External.GetSkinningMode();
	// SKIN_0 has a different vertex layout from SKIN_NONE (-1).
	if (m_skinning >= 0)
	{
		res_name += "_" + xr_string::ToString(m_skinning);
	}

	LPCSTR name = res_name.c_str();
	LPSTR N = LPSTR(name);
	map_VS::iterator I = m_vs.find(N);
	if (I != m_vs.end()) return I->second;
	else
	{
		SVS* _vs = xr_new<SVS>();
		_vs->skinning = m_skinning;
		_vs->dwFlags |= xr_resource_flagged::RF_REGISTERED;
		m_vs.insert(mk_pair(_vs->set_name(name), _vs));
		//_vs->vs				= NULL;
		//_vs->signature		= NULL;
		if (0 == stricmp(_name, "null"))
		{
			return _vs;
		}

		string_path shName;
		{
			const char* pchr = strchr(_name, '(');
			ptrdiff_t size = pchr ? pchr - _name : xr_strlen(_name);
			strncpy(shName, _name, size);
			shName[size] = 0;
		}
		// A tree shader's instanced variant (patch_tree_instancing_shader): the file without the suffix
		bool tree_instanced = false;
		{
			const size_t len = xr_strlen(shName);
			if (len > 7 && !xr_strcmp(shName + len - 7, "_xrinst"))
			{
				shName[len - 7] = 0;
				tree_instanced = true;
			}
		}

		string_path cname;
		strconcat(sizeof(cname), cname, ::Render->getShaderPath(),/*_name*/shName, ".vs");
		const xr_string source = cname; // under $game_shaders$
		FS.update_path(cname, "$game_shaders$", cname);
		//		LPCSTR						target		= NULL;

		// duplicate and zero-terminate
		IReader* file = shader_patches::open(source.c_str());
		//	TODO: DX10: HACK: Implement all shaders. Remove this for PS
		if (!file)
		{
			string1024 tmp;
			xr_sprintf(tmp, "DX10: %s is missing. Replace with stub_default.vs", cname);
			Msg(tmp);
			strconcat(sizeof(cname), cname, ::Render->getShaderPath(), "stub_default", ".vs");
			FS.update_path(cname, "$game_shaders$", cname);
			file = FS.r_open(cname);
		}
		u32 const size = file->length();
		char* const data = (LPSTR)_alloca(size + 1);
		CopyMemory(data, file->pointer(), size);
		data[size] = 0;
		FS.r_close(file);

		// Select target
		LPCSTR c_target = "vs_2_0";
		LPCSTR c_entry = "main";
		if (HW.Caps.geometry_major >= 2) c_target = "vs_2_0";
		else c_target = "vs_1_1";

		if (strstr(data, "main_vs_1_1"))
		{
			c_target = "vs_1_1";
			c_entry = "main_vs_1_1";
		}
		if (strstr(data, "main_vs_2_0"))
		{
			c_target = "vs_2_0";
			c_entry = "main_vs_2_0";
		}

		LPCSTR src = data;
		u32 src_size = size;
		LPCSTR compile_name = name;
#if defined(USE_DX11)
		xr_string patched;
		string_path patched_name;
		if (tree_instanced)
		{
			// A variant made by including another tree shader (#include "deffer_tree_bump.vs"): that one's text in
			// place, its constants are to change too
			xr_string expanded = data;
			for (int depth = 0; depth < 4; ++depth)
			{
				size_t inc = 0, end = xr_string::npos;
				for (; (inc = expanded.find("#include \"", inc)) != xr_string::npos; inc += 10)
				{
					end = expanded.find('"', inc + 10);
					if (end != xr_string::npos && end >= inc + 14 && !expanded.compare(end - 3, 3, ".vs"))
						break;
				}
				if (inc == xr_string::npos)
					break;
				string_path inc_name;
				strconcat(sizeof(inc_name), inc_name, ::Render->getShaderPath(), expanded.substr(inc + 10, end - inc - 10).c_str());
				IReader* R = shader_patches::open(inc_name);
				if (!R)
					break;
				expanded.replace(inc, end + 1 - inc, xr_string((LPCSTR)R->pointer(), R->length()));
				FS.r_close(R);
			}
			if (!patch_tree_instancing_shader(expanded.c_str(), patched))
			{
				Msg("* Tree instancing: %s.vs isn't a shape it can change, its trees are drawn one by one", shName);
				return _vs;
			}
			src = patched.c_str();
			src_size = u32(patched.size());
		}
		else if (patch_grass_jitter_shader(data, patched))
		{
			Msg("* %s.vs: grass jittered over the whole blade while upscaling (r4_upscaler_grass_jitter)", shName);
			src = patched.c_str();
			src_size = u32(patched.size());
			xr_strconcat(patched_name, name, "_nxjit1");
			compile_name = patched_name;
		}
#endif
		HRESULT const _hr = ::Render->shader_compile(compile_name, (DWORD const*)src, src_size, c_entry, c_target,
		                                             D3D10_SHADER_PACK_MATRIX_ROW_MAJOR, (void*&)_vs);

#if defined(USE_DX11)
		// An instanced variant that doesn't compile: its trees are drawn one by one
		if (tree_instanced && FAILED(_hr))
		{
			Msg("! Tree instancing: %s.vs's instanced variant not compiled, its trees are drawn one by one", shName);
			return _vs;
		}
#endif
		VERIFY(SUCCEEDED(_hr));

		CHECK_OR_EXIT(
			!FAILED(_hr),
			make_string("Shader compilation failed, check your log file for additional information.")
		);

		return _vs;
	}
}

void CResourceManager::_DeleteVS(const SVS* vs)
{
	if (0 == (vs->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(creationGuard);
	LPSTR N = LPSTR(*vs->cName);
	map_VS::iterator I = m_vs.find(N);
	if (I != m_vs.end())
	{
		m_vs.erase(I);
		// A shader that wasn't compiled (a tree instancing variant its source couldn't give) has no layouts
		if (!vs->signature)
			return;
		xr_vector<SDeclaration*>::iterator iDecl;
		for (iDecl = v_declarations.begin(); iDecl != v_declarations.end(); ++iDecl)
		{
			xr_map<ID3DBlob*, ID3DInputLayout*>::iterator iLayout;
			iLayout = (*iDecl)->vs_to_layout.find(vs->signature->signature);
			if (iLayout != (*iDecl)->vs_to_layout.end())
			{
				//	Release vertex layout
				_RELEASE(iLayout->second);
				(*iDecl)->vs_to_layout.erase(iLayout);
			}
		}
		return;
	}
	Msg("! ERROR: Failed to find compiled vertex-shader '%s'", *vs->cName);
}

#if defined(USE_DX11)
// The 3DSS lens shaders (any 3DSS for GAMMA version) take their screen position from the render resolution
// (I.hpos.xy * screen_res.zw); drawn after the upscaler, at output resolution, they need nextray_lens_res there (0 without
// it, which leaves them as they were). With r4_upscaler_lens_patch the source is changed so when it's compiled, in memory:
// only that expression in these three shaders, and only when they don't take nextray_lens_res themselves. Patched ones
// compile under their own name, so their cache entry is separate from the stock one
static bool patch_3dss_lens_shader(LPCSTR sh_name, LPCSTR data, xr_string& patched)
{
	extern int ps_r4_upscaler_lens_patch;
	if (!ps_r4_upscaler_lens_patch)
		return false;
	if (stricmp(sh_name, "models_scope_reticle") && stricmp(sh_name, "models_scope_reticle_precise") &&
		stricmp(sh_name, "transparent_hud"))
		return false;
	if (strstr(data, "nextray_lens_res"))
		return false;

	static const char pattern[] = "I.hpos.xy * screen_res.zw";
	static const char replacement[] = "I.hpos.xy * (nextray_lens_res.x > 0 ? nextray_lens_res.zw : screen_res.zw)";
	if (!strstr(data, pattern))
		return false;

	// The declaration first, then the file as it is, its line numbers kept for compile errors
	patched = "uniform float4 nextray_lens_res;\n#line 1\n";
	u32 count = 0;
	for (LPCSTR p = data;;)
	{
		LPCSTR hit = strstr(p, pattern);
		if (!hit)
		{
			patched += p;
			break;
		}
		patched.append(p, hit - p);
		patched += replacement;
		p = hit + sizeof(pattern) - 1;
		++count;
	}
	Msg("* %s.ps: 3DSS lens screen position from the upscaler's output (%u place(s), r4_upscaler_lens_patch)", sh_name,
		count);
	return true;
}
#endif

//--------------------------------------------------------------------------------------------------------------
SPS* CResourceManager::_CreatePS(LPCSTR _name)
{
	PROF_EVENT("_CreatePS");
	xrCriticalSectionGuard guard(creationGuard);
	string_path name;
	xr_strcpy(name, _name);
	if (0 == ::Render->m_MSAASample) xr_strcat(name, "_0");
	if (1 == ::Render->m_MSAASample) xr_strcat(name, "_1");
	if (2 == ::Render->m_MSAASample) xr_strcat(name, "_2");
	if (3 == ::Render->m_MSAASample) xr_strcat(name, "_3");
	if (4 == ::Render->m_MSAASample) xr_strcat(name, "_4");
	if (5 == ::Render->m_MSAASample) xr_strcat(name, "_5");
	if (6 == ::Render->m_MSAASample) xr_strcat(name, "_6");
	if (7 == ::Render->m_MSAASample) xr_strcat(name, "_7");
	LPSTR N = LPSTR(name);
	map_PS::iterator I = m_ps.find(N);
	if (I != m_ps.end()) return I->second;
	else
	{
		SPS* _ps = xr_new<SPS>();
		_ps->dwFlags |= xr_resource_flagged::RF_REGISTERED;
		m_ps.insert(mk_pair(_ps->set_name(name), _ps));
		if (0 == stricmp(_name, "null"))
		{
			_ps->ps = NULL;
			return _ps;
		}

		string_path shName;
		const char* pchr = strchr(_name, '(');
		ptrdiff_t strSize = pchr ? pchr - _name : xr_strlen(_name);
		strncpy(shName, _name, strSize);
		shName[strSize] = 0;

		// Open file
		string_path cname;
		strconcat(sizeof(cname), cname, ::Render->getShaderPath(),/*_name*/shName, ".ps");
		const xr_string source = cname; // under $game_shaders$
		FS.update_path(cname, "$game_shaders$", cname);

		// duplicate and zero-terminate
		IReader* file = shader_patches::open(source.c_str());
		//	TODO: DX10: HACK: Implement all shaders. Remove this for PS
		if (!file)
		{
			string1024 tmp;
			//	TODO: HACK: Test failure
			//Memory.mem_compact();
			xr_sprintf(tmp, "DX10: %s is missing. Replace with stub_default.ps", cname);
			Msg(tmp);
			strconcat(sizeof(cname), cname, ::Render->getShaderPath(), "stub_default", ".ps");
			FS.update_path(cname, "$game_shaders$", cname);
			file = FS.r_open(cname);
		}

		R_ASSERT2(file, cname);
		u32 const size = file->length();
		char* const data = (LPSTR)_alloca(size + 1);
		CopyMemory(data, file->pointer(), size);
		data[size] = 0;
		FS.r_close(file);

		// Select target
		LPCSTR c_target = "ps_2_0";
		LPCSTR c_entry = "main";
		if (strstr(data, "main_ps_1_1"))
		{
			c_target = "ps_1_1";
			c_entry = "main_ps_1_1";
		}
		if (strstr(data, "main_ps_1_2"))
		{
			c_target = "ps_1_2";
			c_entry = "main_ps_1_2";
		}
		if (strstr(data, "main_ps_1_3"))
		{
			c_target = "ps_1_3";
			c_entry = "main_ps_1_3";
		}
		if (strstr(data, "main_ps_1_4"))
		{
			c_target = "ps_1_4";
			c_entry = "main_ps_1_4";
		}
		if (strstr(data, "main_ps_2_0"))
		{
			c_target = "ps_2_0";
			c_entry = "main_ps_2_0";
		}

		LPCSTR src = data;
		u32 src_size = size;
		LPCSTR compile_name = name;
#if defined(USE_DX11)
		xr_string patched;
		string_path patched_name;
		if (patch_3dss_lens_shader(shName, data, patched))
		{
			src = patched.c_str();
			src_size = u32(patched.size());
			xr_strconcat(patched_name, name, "_nxlens1");
			compile_name = patched_name;
		}
#endif

		HRESULT const _hr = ::Render->shader_compile(compile_name, (DWORD const*)src, src_size, c_entry, c_target,
		                                             D3D10_SHADER_PACK_MATRIX_ROW_MAJOR, (void*&)_ps);

		VERIFY(SUCCEEDED(_hr));

		CHECK_OR_EXIT(
			!FAILED(_hr),
			make_string("Shader compilation failed, check your log file for additional information.")
		);

		return _ps;
	}
}

void CResourceManager::_DeletePS(const SPS* ps)
{
	if (0 == (ps->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(creationGuard);
	LPSTR N = LPSTR(*ps->cName);
	map_PS::iterator I = m_ps.find(N);
	if (I != m_ps.end())
	{
		m_ps.erase(I);
		return;
	}
	Msg("! ERROR: Failed to find compiled pixel-shader '%s'", *ps->cName);
}

//--------------------------------------------------------------------------------------------------------------
SGS* CResourceManager::_CreateGS(LPCSTR name)
{
	PROF_EVENT("_CreateGS");
	xrCriticalSectionGuard guard(creationGuard);
	LPSTR N = LPSTR(name);
	map_GS::iterator I = m_gs.find(N);
	if (I != m_gs.end()) return I->second;
	else
	{
		SGS* _gs = xr_new<SGS>();
		_gs->dwFlags |= xr_resource_flagged::RF_REGISTERED;
		m_gs.insert(mk_pair(_gs->set_name(name), _gs));
		if (0 == stricmp(name, "null"))
		{
			_gs->gs = NULL;
			return _gs;
		}

		// Open file
		string_path cname;
		strconcat(sizeof(cname), cname, ::Render->getShaderPath(), name, ".gs");
		const xr_string source = cname; // under $game_shaders$
		FS.update_path(cname, "$game_shaders$", cname);

		// duplicate and zero-terminate
		IReader* file = shader_patches::open(source.c_str());
		//	TODO: DX10: HACK: Implement all shaders. Remove this for PS
		if (!file)
		{
			string1024 tmp;
			//	TODO: HACK: Test failure
			//Memory.mem_compact();
			xr_sprintf(tmp, "DX10: %s is missing. Replace with stub_default.gs", cname);
			Msg(tmp);
			strconcat(sizeof(cname), cname, ::Render->getShaderPath(), "stub_default", ".gs");
			FS.update_path(cname, "$game_shaders$", cname);
			file = FS.r_open(cname);
		}

		R_ASSERT2(file, cname);

		// Select target
		LPCSTR c_target = "gs_4_0";
		LPCSTR c_entry = "main";

		HRESULT const _hr = ::Render->shader_compile(name, (DWORD const*)file->pointer(), file->length(), c_entry,
		                                             c_target, D3D10_SHADER_PACK_MATRIX_ROW_MAJOR, (void*&)_gs);

		VERIFY(SUCCEEDED(_hr));

		FS.r_close(file);

		CHECK_OR_EXIT(
			!FAILED(_hr),
			make_string("Shader compilation failed, check your log file for additional information.")
		);

		return _gs;
	}
}

void CResourceManager::_DeleteGS(const SGS* gs)
{
	if (0 == (gs->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(creationGuard);
	LPSTR N = LPSTR(*gs->cName);
	map_GS::iterator I = m_gs.find(N);
	if (I != m_gs.end())
	{
		m_gs.erase(I);
		return;
	}
	Msg("! ERROR: Failed to find compiled geometry shader '%s'", *gs->cName);
}

//--------------------------------------------------------------------------------------------------------------
static BOOL dcl_equal(D3DVERTEXELEMENT9* a, D3DVERTEXELEMENT9* b)
{
	// check sizes
	u32 a_size = D3DXGetDeclLength(a);
	u32 b_size = D3DXGetDeclLength(b);
	if (a_size != b_size) return FALSE;
	return 0 == memcmp(a, b, a_size * sizeof(D3DVERTEXELEMENT9));
}

SDeclaration* CResourceManager::_CreateDecl(D3DVERTEXELEMENT9* dcl)
{
	xrCriticalSectionGuard guard(creationGuard);
	// Search equal code
	for (u32 it = 0; it < v_declarations.size(); it++)
	{
		SDeclaration* D = v_declarations[it];;
		if (dcl_equal(dcl, &*D->dcl_code.begin())) return D;
	}

	// Create _new
	SDeclaration* D = xr_new<SDeclaration>();
	u32 dcl_size = D3DXGetDeclLength(dcl) + 1;
	//	Don't need it for DirectX 10 here
	//CHK_DX					(HW.pDevice->CreateVertexDeclaration(dcl,&D->dcl));
	D->dcl_code.assign(dcl, dcl + dcl_size);
	dx10BufferUtils::ConvertVertexDeclaration(D->dcl_code, D->dx10_dcl_code);
	D->dwFlags |= xr_resource_flagged::RF_REGISTERED;
	v_declarations.push_back(D);
	return D;
}

void CResourceManager::_DeleteDecl(const SDeclaration* dcl)
{
	if (0 == (dcl->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(creationGuard);
	if (reclaim(v_declarations, dcl)) return;
	Msg("! ERROR: Failed to find compiled vertex-declarator");
}

//--------------------------------------------------------------------------------------------------------------
R_constant_table* CResourceManager::_CreateConstantTable(R_constant_table& C)
{
	if (C.empty())		return NULL;

	xrCriticalSectionGuard guard(creationGuard);

	for (u32 it = 0; it < v_constant_tables.size(); it++)
		if (v_constant_tables[it]->equal(C))
			return v_constant_tables[it];

	auto NewElem = xr_new<R_constant_table>(C);
	//NewElem->_copy(C);
	NewElem->dwFlags |= xr_resource_flagged::RF_REGISTERED;
	v_constant_tables.push_back(NewElem);
	return NewElem;

	return v_constant_tables.back();
}

void CResourceManager::_DeleteConstantTable(const R_constant_table* C)
{
	if (0 == (C->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(creationGuard);
	if (reclaim(v_constant_tables, C)) return;
	Msg("! ERROR: Failed to find compiled constant-table");
}

//--------------------------------------------------------------------------------------------------------------
#ifdef USE_DX11
CRT* CResourceManager::_CreateRT(LPCSTR Name, u32 w, u32 h, D3DFORMAT f, u32 SampleCount, bool useUAV)
#else
CRT* CResourceManager::_CreateRT(LPCSTR Name, u32 w, u32 h, D3DFORMAT f, u32 SampleCount)
#endif
{
	R_ASSERT(Name && Name[0] && w && h);

	// ***** first pass - search already created RT
	LPSTR N = LPSTR(Name);
	xrCriticalSectionGuard guard(creationGuard);
	map_RT::iterator I = m_rtargets.find(N);
	if (I != m_rtargets.end()) return I->second;
	else
	{
		CRT* RT = xr_new<CRT>();
		RT->dwFlags |= xr_resource_flagged::RF_REGISTERED;
		m_rtargets.insert(mk_pair(RT->set_name(Name), RT));
#ifdef USE_DX11
		if (Device.b_is_Ready) RT->create(Name, w, h, f, SampleCount, useUAV);
#else
		if (Device.b_is_Ready) RT->create(Name, w, h, f, SampleCount);
#endif
		return RT;
	}
}

void CResourceManager::_DeleteRT(const CRT* RT)
{
	if (0 == (RT->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	LPSTR N = LPSTR(*RT->cName);
	xrCriticalSectionGuard guard(creationGuard);
	map_RT::iterator I = m_rtargets.find(N);
	if (I != m_rtargets.end())
	{
		m_rtargets.erase(I);
		return;
	}
	Msg("! ERROR: Failed to find render-target '%s'", *RT->cName);
}

//--------------------------------------------------------------------------------------------------------------
void CResourceManager::DBG_VerifyGeoms()
{
	/*
	for (u32 it=0; it<v_geoms.size(); it++)
	{
	SGeometry* G					= v_geoms[it];

	D3DVERTEXELEMENT9		test	[MAX_FVF_DECL_SIZE];
	u32						size	= 0;
	G->dcl->GetDeclaration			(test,(unsigned int*)&size);
	u32 vb_stride					= D3DXGetDeclVertexSize	(test,0);
	u32 vb_stride_cached			= G->vb_stride;
	R_ASSERT						(vb_stride == vb_stride_cached);
	}
	*/
}

SGeometry* CResourceManager::CreateGeom(D3DVERTEXELEMENT9* decl, ID3DVertexBuffer* vb, ID3DIndexBuffer* ib)
{
	xrCriticalSectionGuard guard(creationGuard);
	R_ASSERT(decl && vb);

	SDeclaration* dcl = _CreateDecl(decl);
	u32 vb_stride = D3DXGetDeclVertexSize(decl, 0);

	// ***** first pass - search already loaded shader
	for (u32 it = 0; it < v_geoms.size(); it++)
	{
		SGeometry& G = *(v_geoms[it]);
		if ((G.dcl == dcl) && (G.vb == vb) && (G.ib == ib) && (G.vb_stride == vb_stride)) return v_geoms[it];
	}

	SGeometry* Geom = xr_new<SGeometry>();
	Geom->dwFlags |= xr_resource_flagged::RF_REGISTERED;
	Geom->dcl = dcl;
	Geom->vb = vb;
	Geom->vb_stride = vb_stride;
	Geom->ib = ib;
	v_geoms.push_back(Geom);
	return Geom;
}

SGeometry* CResourceManager::CreateGeom(u32 FVF, ID3DVertexBuffer* vb, ID3DIndexBuffer* ib)
{
	D3DVERTEXELEMENT9 dcl [MAX_FVF_DECL_SIZE];
	xrCriticalSectionGuard guard(creationGuard);
	CHK_DX(D3DXDeclaratorFromFVF(FVF,dcl));
	SGeometry* g = CreateGeom(dcl, vb, ib);
	return g;
}

void CResourceManager::DeleteGeom(const SGeometry* Geom)
{
	if (0 == (Geom->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(creationGuard);
	if (reclaim(v_geoms, Geom)) return;
	Msg("! ERROR: Failed to find compiled geometry-declaration");
}

//--------------------------------------------------------------------------------------------------------------
xr_task_group textures_load_tasks;
#ifdef USE_DX11
namespace tex_streaming { bool eligible(LPCSTR name); } // dx10SH_Texture.cpp
#endif
// Textures created on this thread while set are left unloaded until used (CTexture::m_on_demand)
thread_local bool g_texture_create_on_demand = false;
CTexture* CResourceManager::_CreateTexture(LPCSTR _Name)
{
	PROF_EVENT("_CreateTexture");
	// DBG_VerifyTextures	();
	if (0 == xr_strcmp(_Name, "null")) return 0;
	//Msg("texture %s", _Name);
	R_ASSERT(_Name && _Name[0]);
	string_path Name;
	xr_strcpy(Name, _Name); //. andy if (strext(Name)) *strext(Name)=0;
	xrCriticalSectionGuard guard(creationGuard);
	fix_texture_name(Name);
	// ***** first pass - search already loaded texture
	LPSTR N = LPSTR(Name);
	map_TextureIt I = m_textures.find(N);
	if (I != m_textures.end()) return I->second;
	else
	{
		CTexture* T = xr_new<CTexture>();
		T->dwFlags |= xr_resource_flagged::RF_REGISTERED;
		m_textures.insert(mk_pair(T->set_name(Name), T));
		T->Preload();
		T->m_on_demand = g_texture_create_on_demand;
#ifdef USE_DX11
		T->m_stream_eligible = !T->m_on_demand && tex_streaming::eligible(Name);
#endif
		if (Device.b_is_Ready && !T->m_on_demand)
		{
			static DWORD this_thread_id = 0;
			this_thread_id = GetCurrentThreadId();
			textures_load_tasks.run([=]()
			{
				if (this_thread_id != GetCurrentThreadId()) { PROF_THREAD("X-Ray PPL Thread") }
				T->Load();
			});
		}
		return T;
	}
}

void CResourceManager::_DeleteTexture(const CTexture* T)
{
	// DBG_VerifyTextures	();

	if (0 == (T->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(creationGuard);
	LPSTR N = LPSTR(*T->cName);
	map_Texture::iterator I = m_textures.find(N);
	if (I != m_textures.end())
	{
		m_textures.erase(I);
		return;
	}
	Msg("! ERROR: Failed to find texture surface '%s'", *T->cName);
}

CTexture* CResourceManager::_FindTexture(LPCSTR _Name)
{
	if (!_Name || !_Name[0]) return 0;
	if (0 == xr_strcmp(_Name, "null")) return 0;

	string_path Name;
	xr_strcpy(Name, _Name);
	fix_texture_name(Name);

	xrCriticalSectionGuard guard(creationGuard);
	map_TextureIt I = m_textures.find(LPSTR(Name));
	return I != m_textures.end() ? I->second : 0;
}

#ifdef DEBUG
void	CResourceManager::DBG_VerifyTextures	()
{
	map_Texture::iterator I		= m_textures.begin	();
	map_Texture::iterator E		= m_textures.end	();
	for (; I!=E; I++) 
	{
		R_ASSERT(I->first);
		R_ASSERT(I->second);
		R_ASSERT(I->second->cName);
		R_ASSERT(0==xr_strcmp(I->first,*I->second->cName));
	}
}
#endif

//--------------------------------------------------------------------------------------------------------------
CMatrix* CResourceManager::_CreateMatrix(LPCSTR Name)
{
	R_ASSERT(Name && Name[0]);
	if (0 == stricmp(Name, "$null")) return NULL;

	LPSTR N = LPSTR(Name);
	xrCriticalSectionGuard guard(creationGuard);
	map_Matrix::iterator I = m_matrices.find(N);
	if (I != m_matrices.end()) return I->second;
	else
	{
		CMatrix* M = xr_new<CMatrix>();
		M->dwFlags |= xr_resource_flagged::RF_REGISTERED;
		M->dwReference.store(1, std::memory_order_relaxed);
		m_matrices.insert(mk_pair(M->set_name(Name), M));
		return M;
	}
}

void CResourceManager::_DeleteMatrix(const CMatrix* M)
{
	if (0 == (M->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(creationGuard);
	LPSTR N = LPSTR(*M->cName);
	map_Matrix::iterator I = m_matrices.find(N);
	if (I != m_matrices.end())
	{
		m_matrices.erase(I);
		return;
	}
	Msg("! ERROR: Failed to find xform-def '%s'", *M->cName);
}

//--------------------------------------------------------------------------------------------------------------
CConstant* CResourceManager::_CreateConstant(LPCSTR Name)
{
	R_ASSERT(Name && Name[0]);
	if (0 == stricmp(Name, "$null")) return NULL;

	LPSTR N = LPSTR(Name);
	xrCriticalSectionGuard guard(creationGuard);
	map_Constant::iterator I = m_constants.find(N);
	if (I != m_constants.end()) return I->second;
	else
	{
		CConstant* C = xr_new<CConstant>();
		C->dwFlags |= xr_resource_flagged::RF_REGISTERED;
		C->dwReference.store(1, std::memory_order_relaxed);
		m_constants.insert(mk_pair(C->set_name(Name), C));
		return C;
	}
}

void CResourceManager::_DeleteConstant(const CConstant* C)
{
	if (0 == (C->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(creationGuard);
	LPSTR N = LPSTR(*C->cName);
	map_Constant::iterator I = m_constants.find(N);
	if (I != m_constants.end())
	{
		m_constants.erase(I);
		return;
	}
	Msg("! ERROR: Failed to find R1-constant-def '%s'", *C->cName);
}

//--------------------------------------------------------------------------------------------------------------
bool cmp_tl(const std::pair<u32, ref_texture>& _1, const std::pair<u32, ref_texture>& _2)
{
	return _1.first < _2.first;
}

STextureList* CResourceManager::_CreateTextureList(STextureList& L)
{
	xrCriticalSectionGuard guard(creationGuard);
	std::sort(L.begin(), L.end(), cmp_tl);
	for (u32 it = 0; it < lst_textures.size(); it++)
	{
		STextureList* base = lst_textures[it];
		if (L.equal(*base)) return base;
	}
	STextureList* lst = xr_new<STextureList>(L);
	//lst->_copy(L);
	lst->dwFlags |= xr_resource_flagged::RF_REGISTERED;

	lst_textures.push_back(lst);
	return lst;
}

void CResourceManager::_DeleteTextureList(const STextureList* L)
{
	if (0 == (L->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(creationGuard);
	if (reclaim(lst_textures, L)) return;
	Msg("! ERROR: Failed to find compiled list of textures");
}

//--------------------------------------------------------------------------------------------------------------
SMatrixList* CResourceManager::_CreateMatrixList(SMatrixList& L)
{
	BOOL bEmpty = TRUE;
	for (u32 i = 0; i < L.size(); i++)
		if (L[i])
		{
			bEmpty = FALSE;
			break;
		}
	if (bEmpty) return NULL;

	xrCriticalSectionGuard guard(creationGuard);

	for (u32 it = 0; it < lst_matrices.size(); it++)
	{
		SMatrixList* base = lst_matrices[it];
		if (L.equal(*base)) return base;
	}
	SMatrixList* lst = xr_new<SMatrixList>(L);
	//lst->_copy(L);

	lst->dwFlags |= xr_resource_flagged::RF_REGISTERED;
	lst_matrices.push_back(lst);
	return lst;
}

void CResourceManager::_DeleteMatrixList(const SMatrixList* L)
{
	if (0 == (L->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(creationGuard);
	if (reclaim(lst_matrices, L)) return;
	Msg("! ERROR: Failed to find compiled list of xform-defs");
}

//--------------------------------------------------------------------------------------------------------------
SConstantList* CResourceManager::_CreateConstantList(SConstantList& L)
{
	BOOL bEmpty = TRUE;
	for (u32 i = 0; i < L.size(); i++)
		if (L[i])
		{
			bEmpty = FALSE;
			break;
		}
	if (bEmpty) return NULL;

	xrCriticalSectionGuard guard(creationGuard);

	for (u32 it = 0; it < lst_constants.size(); it++)
	{
		SConstantList* base = lst_constants[it];
		if (L.equal(*base)) return base;
	}
	SConstantList* lst = xr_new<SConstantList>(L);
	//lst->_copy(L);

	lst->dwFlags |= xr_resource_flagged::RF_REGISTERED;
	lst_constants.push_back(lst);
	return lst;
}

void CResourceManager::_DeleteConstantList(const SConstantList* L)
{
	if (0 == (L->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(creationGuard);
	if (reclaim(lst_constants, L)) return;
	Msg("! ERROR: Failed to find compiled list of r1-constant-defs");
}

//--------------------------------------------------------------------------------------------------------------
dx10ConstantBuffer* CResourceManager::_CreateConstantBuffer(ID3DShaderReflectionConstantBuffer* pTable)
{
	VERIFY(pTable);
	xrCriticalSectionGuard guard(creationGuard);
	dx10ConstantBuffer* pTempBuffer = xr_new<dx10ConstantBuffer>(pTable);

	for (u32 it = 0; it < v_constant_buffer.size(); it++)
	{
		dx10ConstantBuffer* buf = v_constant_buffer[it];
		if (pTempBuffer->Similar(*buf))
		{
			xr_delete(pTempBuffer);
			return buf;
		}
	}

	pTempBuffer->dwFlags |= xr_resource_flagged::RF_REGISTERED;
	v_constant_buffer.push_back(pTempBuffer);
	return pTempBuffer;
}

//--------------------------------------------------------------------------------------------------------------
void CResourceManager::_DeleteConstantBuffer(const dx10ConstantBuffer* pBuffer)
{
	if (0 == (pBuffer->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(creationGuard);
	if (reclaim(v_constant_buffer, pBuffer)) return;
	Msg("! ERROR: Failed to find compiled constant buffer");
}

//--------------------------------------------------------------------------------------------------------------
SInputSignature* CResourceManager::_CreateInputSignature(ID3DBlob* pBlob)
{
	VERIFY(pBlob);
	xrCriticalSectionGuard guard(creationGuard);

	for (u32 it = 0; it < v_input_signature.size(); it++)
	{
		SInputSignature* sign = v_input_signature[it];
		if ((pBlob->GetBufferSize() == sign->signature->GetBufferSize()) &&
			(!(memcmp(pBlob->GetBufferPointer(), sign->signature->GetBufferPointer(), pBlob->GetBufferSize()))))
		{
			return sign;
		}
	}

	SInputSignature* pSign = xr_new<SInputSignature>(pBlob);

	pSign->dwFlags |= xr_resource_flagged::RF_REGISTERED;
	v_input_signature.push_back(pSign);

	return pSign;
}

//--------------------------------------------------------------------------------------------------------------
void CResourceManager::_DeleteInputSignature(const SInputSignature* pSignature)
{
	if (0 == (pSignature->dwFlags & xr_resource_flagged::RF_REGISTERED)) return;
	xrCriticalSectionGuard guard(creationGuard);
	if (reclaim(v_input_signature, pSignature)) return;
	Msg("! ERROR: Failed to find compiled constant buffer");
}
