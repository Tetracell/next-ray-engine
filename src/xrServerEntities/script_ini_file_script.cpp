////////////////////////////////////////////////////////////////////////////
//	Module 		: script_ini_file_script.cpp
//	Created 	: 25.06.2004
//  Modified 	: 25.06.2004
//	Author		: Dmitriy Iassenev
//	Description : Script ini file class export
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "script_ini_file.h"

#include "string_table.h"
#include "../xrCore/nextray_version.h"
#include "script_engine.h"
#include "ai_space.h"

using namespace luabind;

CScriptIniFile* get_system_ini()
{
	return ((CScriptIniFile*)pSettings);
}

//Alundaio: The extended ability to reload system ini after application launch
#ifdef INI_FILE_EXTENDED_EXPORTS
CScriptIniFile* reload_system_ini()
{
	pSettings->Destroy(const_cast<CInifile*>(pSettings));
	string_path fname;
	FS.update_path(fname, "$game_config$", "system.ltx");
	CInifile::InvalidateCache();
	pSettings = xr_new<CInifile>(fname);
	return ((CScriptIniFile*)pSettings);
}

void section_for_each(CScriptIniFile* self, ::luabind::functor<bool> functor)
{
	typedef CInifile::Root sections_type;
	sections_type& sections = self->sections();

	// the functor can remove sections so walk a copy of the section list and skip whatever it removed
	xr_vector<shared_str> names;
	names.reserve(sections.size());

	sections_type::const_iterator i = sections.begin();
	sections_type::const_iterator e = sections.end();
	for (; i != e; ++i)
		names.push_back((*i).Name);

	for (const shared_str& name : names)
	{
		if (!self->section_exist((LPCSTR)name.c_str()))
			continue;

		if (functor((LPCSTR)name.c_str()) == true)
			return;
	}
}
#endif
//Alundaio: END

#ifdef XRGAME_EXPORTS
CScriptIniFile* get_game_ini()
{
	return ((CScriptIniFile*)pGameIni);
}
#endif // XRGAME_EXPORTS

bool r_line(CScriptIniFile* self, LPCSTR S, int L, ::luabind::internal_string&N, ::luabind::internal_string& V)
{
	THROW3(self->section_exist(S), "Cannot find section", S);
	THROW2((int)self->line_count(S) > L, "Invalid line number");

	N = "";
	V = "";

	LPCSTR n, v;
	bool result = !!self->r_line(S, L, &n, &v);
	if (!result)
		return (false);

	N = n;
	if (v)
		V = v;
	return (true);
}

// _g.script's r_string_ex / r_bool_ex / r_float_ex as one call (used by _g_patches.script): 0 when the section or
// the line doesn't exist, 1 when the value is empty (r_string gives nil), 2 and the value. The same checks as the
// script's section_exist, line_exist and r_string calls, which loading GAMMA's scripts makes ~10^5 times. The value
// comes back like r_line's (an out value), called as ini:r_value_ex(section, line, "")
int r_value_ex(CScriptIniFile* self, LPCSTR S, LPCSTR L, ::luabind::internal_string& V)
{
	V = "";
	// The section once, and the line in it: section_exist, line_exist and r_string looked the section up seven times.
	// The same answer for names r_section finds as they are (lower case) and a non-empty line name
	if (S && L && L[0])
	{
		bool lower = true;
		size_t n = 0;
		for (; S[n]; n++)
			if (S[n] >= 'A' && S[n] <= 'Z')
				lower = false;
		if (lower && n > 0 && n < 256)
		{
			const CInifile::Sect* sect = self->find_section(S);
			if (!sect)
				return 0;
			auto A = std::lower_bound(sect->Data.begin(), sect->Data.end(), L, CInifile::item_comparator());
			if (!(A != sect->Data.end() && xr_strcmp(*A->first, L) == 0))
				return 0;
			LPCSTR value = *A->second;
			if (!value)
				return 1;
			V = value;
			return 2;
		}
	}
	if (!self->section_exist(S) || !self->line_exist(S, L))
		return 0;
	LPCSTR value = self->r_string(S, L);
	if (!value)
		return 1;
	V = value;
	return 2;
}

#pragma warning(push)
#pragma warning(disable:4238)
CScriptIniFile* create_ini_file(LPCSTR ini_string)
{
	return (
		(CScriptIniFile*)
		xr_new<CInifile>(
			&IReader(
				(void*)ini_string,
				xr_strlen(ini_string)
			),
			FS.get_path("$game_config$")->m_Path
		)
	);
}
#pragma warning(pop)

// demonized: get modded exes version
int get_modded_exes_version() {
	LPSTR month_id[12] =
	{
		"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};

	LPCSTR build_date = __DATE__;

	int days;
	int months = 0;
	int years;
	string16 month;
	string256 buffer;
	xr_strcpy(buffer, build_date);
	sscanf(buffer, "%s %d %d", month, &days, &years);

	for (int i = 0; i < 12; i++)
	{
		if (_stricmp(month_id[i], month))
			continue;

		months = i;
		break;
	}

	months++;

	// Convert the parsed date to the desired integer format
	int result = years * 10000 + months * 100 + days;
	return result;
}

LPCSTR get_modded_exes_name() {
	shared_str result = "Modded Exes MT-TEST";
	return result.c_str();
}

LPCSTR get_nextray_version() { return NEXTRAY_VERSION; }

// demonized: get modded exes version
xr_string get_modded_exes_version_string() {
	std::string s = std::to_string(get_modded_exes_version());
	s.insert(4, 1, '.').insert(7, 1, '.');
	return xr_string(s.c_str());
}

::luabind::object get_string_table() {
	auto pData = CStringTable::getPData();
	::luabind::object table = ::luabind::newtable(ai().script_engine().lua());
	if (!pData) {
		return table;
	}

	for (const auto& v : pData->m_StringTable) {
		table[v.first.c_str()] = v.second.c_str();
	}
	
	return table;
}

#pragma optimize("s",on)
void CScriptIniFile::script_register(lua_State* L)
{
	module(L)
	[
		class_<CScriptIniFile>("ini_file")
		.def(constructor<LPCSTR>())
		//Alundaio: Extend script ini file
#ifdef INI_FILE_EXTENDED_EXPORTS
		.def(constructor<IReader*, LPCSTR>())
		.def(constructor<LPCSTR, BOOL, BOOL, BOOL, LPCSTR>())
		.def("w_bool", &CScriptIniFile::w_bool)
		.def("w_color", &CScriptIniFile::w_color)
		.def("w_fcolor", &CScriptIniFile::w_fcolor)
		.def("w_float", &CScriptIniFile::w_float)
		.def("w_fvector2", &CScriptIniFile::w_fvector2)
		.def("w_fvector3", &CScriptIniFile::w_fvector3)
		.def("w_fvector4", &CScriptIniFile::w_fvector4)
		.def("w_s16", &CScriptIniFile::w_s16)
		.def("w_s32", &CScriptIniFile::w_s32)
		.def("w_s64", &CScriptIniFile::w_s64)
		.def("w_s8", &CScriptIniFile::w_s8)
		.def("w_string", &CScriptIniFile::w_string)
		.def("w_u16", &CScriptIniFile::w_u16)
		.def("w_u32", &CScriptIniFile::w_u32)
		.def("w_u64", &CScriptIniFile::w_u64)
		.def("w_u8", &CScriptIniFile::w_u8)
		.def("save_as", &CScriptIniFile::save_as)
		.def("save_at_end", &CScriptIniFile::save_at_end)
		.def("remove_line", &CScriptIniFile::remove_line)
		.def("remove_section", &CScriptIniFile::remove_section)
		.def("set_override_names", &CScriptIniFile::set_override_names)
		.def("section_count", &CScriptIniFile::section_count)
		.def("section_for_each", &::section_for_each)
		.def("set_readonly", &CScriptIniFile::set_readonly)
#endif
		//Alundaio: END
		.def("section_exist", &CScriptIniFile::section_exist)
		.def("line_exist", &CScriptIniFile::line_exist)
		.def("r_clsid", &CScriptIniFile::r_clsid)
		.def("r_bool", &CScriptIniFile::r_bool)
		.def("r_token", &CScriptIniFile::r_token)
		.def("r_string_wq", &CScriptIniFile::r_string_wb)
		.def("line_count", &CScriptIniFile::line_count)
		.def("r_string", &CScriptIniFile::r_string)
		.def("r_u32", &CScriptIniFile::r_u32)
		.def("r_s32", &CScriptIniFile::r_s32)
		.def("r_float", &CScriptIniFile::r_float)
		.def("r_vector", &CScriptIniFile::r_fvector3)
		.def("r_vector4", &CScriptIniFile::r_fvector4)
		.def("close", &CScriptIniFile::close)
		.def("r_line", &::r_line, out_value<4>() + out_value<5>())
		.def("r_value_ex", &::r_value_ex, out_value<4>())

		// demonized: new exports
		.def("get_filename", &CScriptIniFile::fname)
		.def("dltx_print", &CScriptIniFile::DLTX_print)
		.def("dltx_get_filename_of_line", &CScriptIniFile::DLTX_getFilenameOfLine)
		.def("dltx_get_section", &CScriptIniFile::DLTX_scriptGetSection)
		.def("dltx_is_override", &CScriptIniFile::DLTX_isOverride),

		def("system_ini", &get_system_ini),
		//Alundaio: extend
#ifdef INI_FILE_EXTENDED_EXPORTS
		def("reload_system_ini", &reload_system_ini),
#endif
		//Alundaio:: END
#ifdef XRGAME_EXPORTS
		def("game_ini", &get_game_ini),
#endif // XRGAME_EXPORTS
		def("create_ini_file", &create_ini_file, adopt<result>()),

		// demonized: get modded exes version
		def("get_modded_exes_version", &get_modded_exes_version),
		def("get_modded_exes_name", &get_modded_exes_name),
		def("get_nextray_version", &get_nextray_version),

		// demonized: get translation strings table
		def("get_string_table", &get_string_table)
	];
}
