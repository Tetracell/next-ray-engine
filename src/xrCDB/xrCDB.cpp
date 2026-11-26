// xrCDB.cpp : Defines the entry point for the DLL application.
//

#include "stdafx.h"
#pragma hdrstop

#include "xrCDB.h"
#include "profiler.h"

#ifdef USE_ARENA_ALLOCATOR
static const u32	s_arena_size = (128+16)*1024*1024;
static char			s_fake_array[s_arena_size];
//doug_lea_allocator	g_collision_allocator( s_fake_array, s_arena_size, "collision" );
#endif // #ifdef USE_ARENA_ALLOCATOR

namespace Opcode
{
#	include "OPC_TreeBuilders.h"
} // namespace Opcode

using namespace CDB;
using namespace Opcode;

//BOOL APIENTRY DllMain( HANDLE hModule, 
BOOL DllMainIgnore1(HANDLE hModule,
                    u32 ul_reason_for_call,
                    LPVOID lpReserved
)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}

// Collision model cache: building the level's collision tree took ~3 s on every level load, and the level loader waits
// for it. It depends only on the vertices and triangles, so the finished tree is kept in appdata\cform_cache, under a
// fingerprint of exactly that data: a level a mod changes gets a new fingerprint, is built once and cached again.
// Child links are stored as node indices; a file that doesn't match in any way is ignored and the tree built as before.
namespace cform_cache
{
	const u32 kMagic = 0x42444358; // "XCDB"
	const u32 kVersion = 1;
	const int kMinTris = 50000; // the level collision model, small models build fast enough
	const u64 kMaxBytes = 2ull << 30; // ~120 MB per level of GAMMA (40 bytes per triangle): the ~16 levels used last

	struct header
	{
		u32 magic, version, node_size, nb_nodes;
		u32 verts_count, tris_count;
		u64 key;
	};

	static u64 hash_bytes(const void* data, size_t bytes, u64 h)
	{
		const u64* p = (const u64*)data;
		const size_t n = bytes / 8;
		for (size_t i = 0; i < n; i++)
		{
			h ^= p[i];
			h *= 0x100000001b3ull;
			h ^= h >> 29;
		}
		const u8* t = (const u8*)(p + n);
		for (size_t i = 0; i < (bytes & 7); i++)
		{
			h ^= t[i];
			h *= 0x100000001b3ull;
		}
		return h;
	}

	static bool file_name(u64 key, string_path& out, string_path& dir)
	{
		if (!FS.path_exist("$app_data_root$"))
			return false;
		xr_strcpy(dir, FS.get_path("$app_data_root$")->m_Path);
		xr_strcat(dir, "cform_cache\\");
		xr_sprintf(out, "%scform_%016llx.bin", dir, key);
		return true;
	}

	// Least recently used files go first when they're more than kMaxBytes together
	static void prune(const char* dir)
	{
		string_path mask;
		xr_sprintf(mask, "%s*.bin", dir);
		struct item { FILETIME t; xr_string name; u64 size; };
		xr_vector<item> files;
		WIN32_FIND_DATAA fd;
		HANDLE h = FindFirstFileA(mask, &fd);
		if (h == INVALID_HANDLE_VALUE)
			return;
		do
			files.push_back({ fd.ftLastWriteTime, fd.cFileName, (u64(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow });
		while (FindNextFileA(h, &fd));
		FindClose(h);
		u64 total = 0;
		for (const item& f : files)
			total += f.size;
		std::sort(files.begin(), files.end(), [](const item& a, const item& b) { return CompareFileTime(&a.t, &b.t) < 0; });
		for (size_t i = 0; total > kMaxBytes && i + 1 < files.size(); i++)
		{
			string_path f;
			xr_sprintf(f, "%s%s", dir, files[i].name.c_str());
			if (DeleteFileA(f))
				total -= files[i].size;
		}
	}

	// A file read back counts as used: its time is what prune() keeps the recent levels by
	static void touch(const char* name)
	{
		HANDLE h = CreateFileA(name, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return;
		FILETIME now;
		GetSystemTimeAsFileTime(&now);
		SetFileTime(h, nullptr, nullptr, &now);
		CloseHandle(h);
	}

	static AABBNoLeafTree* load(u64 key, int verts_count, int tris_count)
	{
		string_path name, dir;
		if (!file_name(key, name, dir))
			return nullptr;
		FILE* f = fopen(name, "rb");
		if (!f)
			return nullptr;
		header h = {};
		AABBNoLeafNode* nodes = nullptr;
		bool ok = fread(&h, sizeof(h), 1, f) == 1 && h.magic == kMagic && h.version == kVersion && h.key == key &&
			h.node_size == sizeof(AABBNoLeafNode) && h.verts_count == u32(verts_count) && h.tris_count == u32(tris_count) &&
			h.nb_nodes == u32(tris_count - 1);
		if (ok)
		{
			nodes = CALLOC(AABBNoLeafNode, h.nb_nodes);
			ok = nodes && fread(nodes, sizeof(AABBNoLeafNode), h.nb_nodes, f) == h.nb_nodes;
		}
		fclose(f);
		// Indices back to pointers, checking every link: a damaged file must not crash collision queries
		for (u32 i = 0; ok && i < h.nb_nodes; i++)
		{
			uintptr_t* links[2] = { &nodes[i].mData, &nodes[i].mData2 };
			for (uintptr_t* l : links)
			{
				if (*l & 1)
					ok = ok && (*l >> 1) < uintptr_t(tris_count);
				else
				{
					const uintptr_t idx = *l >> 1;
					ok = ok && idx > i && idx < h.nb_nodes;
					*l = (uintptr_t)&nodes[ok ? idx : 0];
				}
			}
		}
		if (!ok)
		{
			if (nodes)
				CFREE(nodes);
			Msg("! CDB: collision model cache file %s doesn't match, building", name);
			return nullptr;
		}
		touch(name);
		AABBNoLeafTree* tree = CNEW(AABBNoLeafTree)();
		tree->AdoptNodes(nodes, h.nb_nodes);
		return tree;
	}

	static void save(u64 key, int verts_count, int tris_count, const AABBNoLeafTree* tree)
	{
		string_path name, dir, tmp;
		if (!file_name(key, name, dir))
			return;
		CreateDirectoryA(dir, nullptr);
		xr_sprintf(tmp, "%s.tmp", name);
		FILE* f = fopen(tmp, "wb");
		if (!f)
			return;
		const AABBNoLeafNode* nodes = tree->GetNodes();
		header h = { kMagic, kVersion, sizeof(AABBNoLeafNode), tree->GetNbNodes(), u32(verts_count), u32(tris_count), key };
		bool ok = fwrite(&h, sizeof(h), 1, f) == 1;
		// Pointers to node indices, in chunks
		xr_vector<AABBNoLeafNode> chunk;
		const u32 kChunk = 65536;
		for (u32 i = 0; ok && i < h.nb_nodes; i += kChunk)
		{
			const u32 n = _min(kChunk, h.nb_nodes - i);
			chunk.assign(nodes + i, nodes + i + n);
			for (AABBNoLeafNode& c : chunk)
			{
				if (!(c.mData & 1))
					c.mData = uintptr_t((const AABBNoLeafNode*)c.mData - nodes) << 1;
				if (!(c.mData2 & 1))
					c.mData2 = uintptr_t((const AABBNoLeafNode*)c.mData2 - nodes) << 1;
			}
			ok = fwrite(chunk.data(), sizeof(AABBNoLeafNode), n, f) == n;
		}
		ok = (fclose(f) == 0) && ok;
		if (!ok || !MoveFileExA(tmp, name, MOVEFILE_REPLACE_EXISTING))
		{
			DeleteFileA(tmp);
			return;
		}
		prune(dir);
	}
}

// Model building
MODEL::MODEL()
#ifdef PROFILE_CRITICAL_SECTIONS
	:cs(MUTEX_PROFILE_ID(MODEL))
#endif // PROFILE_CRITICAL_SECTIONS
{
	tree = 0;
	tris = 0;
	tris_count = 0;
	verts = 0;
	verts_count = 0;
	status = S_INIT;
}

MODEL::~MODEL()
{
	syncronize(); // maybe model still in building
	status = S_INIT;
	CDELETE(tree);
	CFREE(tris);
	tris_count = 0;
	CFREE(verts);
	verts_count = 0;
}

void MODEL::build(Fvector* V, int Vcnt, TRI* T, int Tcnt, build_callback* bc, void* bcp)
{
	build_internal(V, Vcnt, T, Tcnt, bc, bcp);
}

void MODEL::build_internal(Fvector* V, int Vcnt, TRI* T, int Tcnt, build_callback* bc, void* bcp)
{
	build_arrays(V, Vcnt, T, Tcnt, bc, bcp);
	build_tree();
}

void MODEL::build_arrays(Fvector* V, int Vcnt, TRI* T, int Tcnt, build_callback* bc, void* bcp)
{
	PROF_EVENT();

	R_ASSERT(S_INIT == status);
	R_ASSERT((Vcnt>=4)&&(Tcnt>=2));

	// verts
	status = S_BUILD;
	verts_count = Vcnt;
	verts = CALLOC(Fvector, verts_count);
	CopyMemory(verts, V, verts_count*sizeof(Fvector));

	// tris
	tris_count = Tcnt;
	tris = CALLOC(TRI, tris_count);
	CopyMemory(tris, T, tris_count*sizeof(TRI));

	// callback
	if (bc) bc(verts, Vcnt, tris, Tcnt, bcp);
}

void MODEL::build_tree()
{
	PROF_EVENT();

	// Allocate temporary "OPCODE" tris + convert tris to 'pointer' form
	u32* temp_tris = CALLOC(u32, tris_count*3);
	if (0 == temp_tris)
		return;
	u32* temp_ptr = temp_tris;
	for (int i = 0; i < tris_count; i++)
	{
		*temp_ptr++ = tris[i].verts[0];
		*temp_ptr++ = tris[i].verts[1];
		*temp_ptr++ = tris[i].verts[2];
	}

	// The level's collision model from the cache (cform_cache above)
	const bool cacheable = tris_count >= cform_cache::kMinTris;
	u64 cache_key = 0;
	CTimer cache_timer;
	if (cacheable)
	{
		cache_timer.Start();
		cache_key = cform_cache::hash_bytes(verts, verts_count * sizeof(Fvector), 0xcbf29ce484222325ull ^ cform_cache::kVersion);
		cache_key = cform_cache::hash_bytes(temp_tris, tris_count * 3 * sizeof(u32), cache_key);
		if (AABBNoLeafTree* cached = cform_cache::load(cache_key, verts_count, tris_count))
		{
			tree = CNEW(OPCODE_Model)();
			tree->AdoptNoLeafTree(cached);
			CFREE(temp_tris);
			Msg("* CDB: collision model (%d triangles) from the cache in %.0f ms", tris_count, cache_timer.GetElapsed_sec() * 1000.f);
			status = S_READY;
			return;
		}
	}

	// Build a non quantized no-leaf tree
	OPCODECREATE OPCC;
	OPCC.NbTris = tris_count;
	OPCC.NbVerts = verts_count;
	OPCC.Tris = (unsigned*)temp_tris;
	OPCC.Verts = (Point*)verts;
	OPCC.Rules = SPLIT_COMPLETE | SPLIT_SPLATTERPOINTS | SPLIT_GEOMCENTER;
	OPCC.NoLeaf = true;
	OPCC.Quantized = false;
	// if (Memory.debug_mode) OPCC.KeepOriginal = true;

	tree = CNEW(OPCODE_Model)();
	if (!tree->Build(OPCC))
	{
		// Do not free verts/tris here. build_arrays already handed them out and
		// callers may hold pointers into them. status stays S_BUILD, so the query
		// paths still refuse to use the tree.
		CFREE(temp_tris);
		return;
	};

	// Free temporary tris
	CFREE(temp_tris);

	if (cacheable)
	{
		const float built_ms = cache_timer.GetElapsed_sec() * 1000.f;
		cform_cache::save(cache_key, verts_count, tris_count, (const AABBNoLeafTree*)tree->GetTree());
		Msg("* CDB: collision model (%d triangles) built in %.0f ms, cached", tris_count, built_ms);
	}

	status = S_READY;
}

u32 MODEL::memory()
{
	if (S_BUILD == status)
	{
		Msg("! xrCDB: model still isn't ready");
		return 0;
	}
	u32 V = verts_count * sizeof(Fvector);
	u32 T = tris_count * sizeof(TRI);
	return tree->GetUsedBytes() + V + T + sizeof(*this) + sizeof(*tree);
}

// This is the constructor of a class that has been exported.
// see xrCDB.h for the class definition
COLLIDER::COLLIDER()
{
	ray_mode = 0;
	box_mode = 0;
	frustum_mode = 0;
	obb_mode = 0;
}

COLLIDER::~COLLIDER()
{
	r_free();
}

RESULT& COLLIDER::r_add()
{
	rd.push_back(RESULT());
	return rd.back();
}

void COLLIDER::r_free()
{
	rd.clear_and_free();
}
