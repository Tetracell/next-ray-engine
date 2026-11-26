// Texture.cpp: implementation of the CTexture class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#pragma hdrstop

#pragma warning(disable:4995)
#include <d3dx9.h>
#pragma warning(default:4995)

#include <D3DX10Tex.h>

#include "../xrRender/dxRenderDeviceRender.h"

// #include "std_classes.h"
// #include "xr_avi.h"

void fix_texture_name(LPSTR fn)
{
	LPSTR _ext = strext(fn);
	if (_ext &&
		(0 == stricmp(_ext, ".tga") ||
			0 == stricmp(_ext, ".dds") ||
			0 == stricmp(_ext, ".bmp") ||
			0 == stricmp(_ext, ".ogm") ||
            0 == stricmp(_ext, ".gif")))
		*_ext = 0;
}

int get_texture_load_lod(LPCSTR fn)
{
	// Scope reticles are magnified on screen: with fewer mips they come out blurred and pixelated. fn is lowercase
	static const char* full_detail[] = { "reticle", "crosshair", "bonus_sights", "\\sights\\", "/sights/" };
	for (const char* part : full_detail)
		if (strstr(fn, part))
			return 0;

	CInifile::Sect& sect = pSettings->r_section("reduce_lod_texture_list");
	CInifile::SectCIt it_ = sect.Data.begin();
	CInifile::SectCIt it_e_ = sect.Data.end();

	ENGINE_API bool is_enough_address_space_available();
	static bool enough_address_space_available = is_enough_address_space_available();

	CInifile::SectCIt it = it_;
	CInifile::SectCIt it_e = it_e_;

	for (; it != it_e; ++it)
	{
		if (strstr(fn, it->first.c_str()))
		{
			if (psTextureLOD < 1)
			{
				if (enough_address_space_available)
					return 0;
				else
					return 1;
			}
			else if (psTextureLOD < 3)
				return 1;
			else
				return 2;
		}
	}

	if (psTextureLOD < 2)
	{
		//		if ( enough_address_space_available )
		return 0;
		//		else
		//			return 1;
	}
	else if (psTextureLOD < 4)
		return 1;
	else
		return 2;
}

u32 calc_texture_size(int lod, u32 mip_cnt, u32 orig_size)
{
	if (1 == mip_cnt)
		return orig_size;

	int _lod = lod;
	float res = float(orig_size);

	while (_lod > 0)
	{
		--_lod;
		res -= res / 1.333f;
	}
	return iFloor(res);
}

const float _BUMPHEIGH = 8.f;
//////////////////////////////////////////////////////////////////////
// Utility pack
//////////////////////////////////////////////////////////////////////
IC u32 GetPowerOf2Plus1(u32 v)
{
	u32 cnt = 0;
	while (v)
	{
		v >>= 1;
		cnt++;
	};
	return cnt;
}

IC void Reduce(int& w, int& h, int& l, int& skip)
{
	while ((l > 1) && skip)
	{
		w /= 2;
		h /= 2;
		l -= 1;

		skip--;
	}
	if (w < 1) w = 1;
	if (h < 1) h = 1;
}

IC void Reduce(UINT& w, UINT& h, int l, int skip)
{
	while ((l > 1) && skip)
	{
		w /= 2;
		h /= 2;
		l -= 1;

		skip--;
	}
	if (w < 1) w = 1;
	if (h < 1) h = 1;
}

void TW_Save(ID3DTexture2D* T, LPCSTR name, LPCSTR prefix, LPCSTR postfix)
{
	string256 fn;
	strconcat(sizeof(fn), fn, name, "_", prefix, "-", postfix);
	for (int it = 0; it < int(xr_strlen(fn)); it++)
		if ('\\' == fn[it]) fn[it] = '_';
	string256 fn2;
	strconcat(sizeof(fn2), fn2, "debug\\", fn, ".dds");
	Log("* debug texture save: ", fn2);
#if defined(USE_DX12)
	Msg("! debug texture save is not available in the DX12 renderer: %s", fn2);
#elif defined(USE_DX11)
	R_CHK(D3DX11SaveTextureToFile(HW.pContext, T, D3DX11_IFF_DDS, fn2));
#else
	R_CHK(D3DX10SaveTextureToFile(T, D3DX10_IFF_DDS, fn2));
#endif
}

#if defined(USE_DX11) && !defined(USE_DX12)
// texture_lod for DDS files: D3DX11 ignores LoadInfo.FirstMipLevel and the reduced size and always creates the
// full texture, so the mip levels are skipped here instead by creating the texture from the file's own data,
// starting at a smaller level. Only for layouts that can be uploaded as they are; anything else returns null and
// goes through D3DX like before.
namespace dds_skip
{
	struct PixelFormat
	{
		u32 size, flags, fourcc, bits, r_mask, g_mask, b_mask, a_mask;
	};

	struct Header
	{
		u32 size, flags, height, width, pitch, depth, mips;
		u32 reserved1[11];
		PixelFormat pf;
		u32 caps, caps2, caps3, caps4, reserved2;
	};

	struct HeaderDX10
	{
		u32 format, dimension, misc, array_size, misc2;
	};

	const u32 kMagic = MAKEFOURCC('D', 'D', 'S', ' ');
	const u32 kFourCC = 0x4, kRGB = 0x40, kAlphaPixels = 0x1;
	const u32 kCubemap = 0x200, kAllFaces = 0xFC00, kVolume = 0x200000;

	// Block size in bytes for BC formats, pixel size otherwise; 0 for formats not handled here
	u32 element_bytes(DXGI_FORMAT f, bool& bc)
	{
		bc = true;
		switch (f)
		{
		case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
		case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
			return 8;
		case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
		case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
		case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
		case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
		case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
			return 16;
		}
		bc = false;
		switch (f)
		{
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
			return 16;
		case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
			return 8;
		case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R11G11B10_FLOAT:
		case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R32_FLOAT:
			return 4;
		case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM:
			return 2;
		case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_A8_UNORM:
			return 1;
		}
		return 0;
	}

	// Format of a file without the DX10 header, from its pixel format block. Uncompressed data is described as
	// it lies in the file (D3DX would swizzle A8R8G8B8 to RGBA, here it stays BGRA)
	DXGI_FORMAT legacy_format(const PixelFormat& pf)
	{
		if (pf.flags & kFourCC)
		{
			switch (pf.fourcc)
			{
			case MAKEFOURCC('D', 'X', 'T', '1'): return DXGI_FORMAT_BC1_UNORM;
			case MAKEFOURCC('D', 'X', 'T', '3'): return DXGI_FORMAT_BC2_UNORM;
			case MAKEFOURCC('D', 'X', 'T', '5'): return DXGI_FORMAT_BC3_UNORM;
			case MAKEFOURCC('A', 'T', 'I', '1'): case MAKEFOURCC('B', 'C', '4', 'U'): return DXGI_FORMAT_BC4_UNORM;
			case MAKEFOURCC('B', 'C', '4', 'S'): return DXGI_FORMAT_BC4_SNORM;
			case MAKEFOURCC('A', 'T', 'I', '2'): case MAKEFOURCC('B', 'C', '5', 'U'): return DXGI_FORMAT_BC5_UNORM;
			case MAKEFOURCC('B', 'C', '5', 'S'): return DXGI_FORMAT_BC5_SNORM;
			}
			return DXGI_FORMAT_UNKNOWN; // DXT2/DXT4 (premultiplied) and D3DFMT codes stay with D3DX
		}
		if ((pf.flags & kRGB) && pf.bits == 32)
		{
			const bool alpha = (pf.flags & kAlphaPixels) && pf.a_mask == 0xff000000;
			if (pf.r_mask == 0x00ff0000 && pf.g_mask == 0x0000ff00 && pf.b_mask == 0x000000ff)
				return alpha ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_B8G8R8X8_UNORM;
			if (pf.r_mask == 0x000000ff && pf.g_mask == 0x0000ff00 && pf.b_mask == 0x00ff0000 && alpha)
				return DXGI_FORMAT_R8G8B8A8_UNORM;
		}
		return DXGI_FORMAT_UNKNOWN;
	}

	size_t mip_bytes(u32 w, u32 h, u32 elem, bool bc, u32& row_pitch)
	{
		if (bc)
		{
			row_pitch = _max(1u, (w + 3) / 4) * elem;
			return size_t(row_pitch) * _max(1u, (h + 3) / 4);
		}
		row_pitch = w * elem;
		return size_t(row_pitch) * h;
	}

	// skip: mip levels to drop, at most as many as leave one level. Returns the texture and the levels skipped
	ID3DBaseTexture* load(const void* data, size_t length, int skip, bool staging, u32& mips_out, int& skipped)
	{
		const u8* bytes = static_cast<const u8*>(data);
		if (skip <= 0 || length < 4 + sizeof(Header) || *reinterpret_cast<const u32*>(bytes) != kMagic)
			return nullptr;

		const Header& hdr = *reinterpret_cast<const Header*>(bytes + 4);
		if (hdr.size != sizeof(Header) || hdr.pf.size != sizeof(PixelFormat) || (hdr.caps2 & kVolume))
			return nullptr;

		size_t offset = 4 + sizeof(Header);
		DXGI_FORMAT format;
		u32 layers = 1;
		bool cube = false;
		if ((hdr.pf.flags & kFourCC) && hdr.pf.fourcc == MAKEFOURCC('D', 'X', '1', '0'))
		{
			if (length < offset + sizeof(HeaderDX10))
				return nullptr;
			const HeaderDX10& dx10 = *reinterpret_cast<const HeaderDX10*>(bytes + offset);
			offset += sizeof(HeaderDX10);
			if (dx10.dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D || !dx10.array_size)
				return nullptr;
			format = DXGI_FORMAT(dx10.format);
			cube = (dx10.misc & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
			layers = dx10.array_size * (cube ? 6 : 1);
		}
		else
		{
			format = legacy_format(hdr.pf);
			if (hdr.caps2 & kCubemap)
			{
				if ((hdr.caps2 & kAllFaces) != kAllFaces)
					return nullptr;
				cube = true;
				layers = 6;
			}
		}

		bool bc = false;
		const u32 elem = element_bytes(format, bc);
		const u32 width = hdr.width, height = hdr.height, mips = _max(hdr.mips, 1u);
		if (!elem || !width || !height || mips > 16)
			return nullptr;

		// The data must be exactly the mip chains of all layers, tightly packed. Anything else (padding,
		// conversions D3DX would do) means the layout isn't understood here
		size_t chain = 0;
		u32 pitch;
		for (u32 m = 0; m < mips; ++m)
			chain += mip_bytes(_max(width >> m, 1u), _max(height >> m, 1u), elem, bc, pitch);
		if (length - offset != chain * layers)
			return nullptr;

		// Keep at least one level, and block-compressed top levels a multiple of 4 as D3D requires
		skip = _min(skip, int(mips) - 1);
		while (skip > 0 && bc && (((width >> skip) & 3) || ((height >> skip) & 3)))
			--skip;
		if (skip <= 0)
			return nullptr;

		const u32 new_mips = mips - u32(skip);
		xr_vector<D3D11_SUBRESOURCE_DATA> init(size_t(new_mips) * layers);
		const u8* src = bytes + offset;
		for (u32 l = 0; l < layers; ++l)
		{
			for (u32 m = 0; m < mips; ++m)
			{
				const size_t size = mip_bytes(_max(width >> m, 1u), _max(height >> m, 1u), elem, bc, pitch);
				if (m >= u32(skip))
				{
					D3D11_SUBRESOURCE_DATA& sd = init[l * new_mips + (m - skip)];
					sd.pSysMem = src;
					sd.SysMemPitch = pitch;
					sd.SysMemSlicePitch = UINT(size);
				}
				src += size;
			}
		}

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = _max(width >> skip, 1u);
		desc.Height = _max(height >> skip, 1u);
		desc.MipLevels = new_mips;
		desc.ArraySize = layers;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
		if (staging)
		{
			desc.Usage = D3D11_USAGE_STAGING;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		}
		else
		{
			desc.Usage = D3D11_USAGE_IMMUTABLE;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		}

		ID3D11Texture2D* texture = nullptr;
		if (FAILED(HW.pDevice->CreateTexture2D(&desc, init.data(), &texture)))
			return nullptr;

		mips_out = new_mips;
		skipped = skip;
		return texture;
	}
}
#endif

/*
ID3DTexture2D*	TW_LoadTextureFromTexture
(
 ID3DTexture2D*		t_from,
 D3DFORMAT&				t_dest_fmt,
 int						levels_2_skip,
 u32&					w,
 u32&					h
 )
{
	// Calculate levels & dimensions
	ID3DTexture2D*		t_dest			= NULL;
	D3DSURFACE_DESC			t_from_desc0	;
	R_CHK					(t_from->GetLevelDesc	(0,&t_from_desc0));
	int levels_exist		= t_from->GetLevelCount();
	int top_width			= t_from_desc0.Width;
	int top_height			= t_from_desc0.Height;
	Reduce					(top_width,top_height,levels_exist,levels_2_skip);

	// Create HW-surface
	if (D3DX_DEFAULT==t_dest_fmt)	t_dest_fmt = t_from_desc0.Format;
	R_CHK					(D3DXCreateTexture(
		HW.pDevice,
		top_width,top_height,
		levels_exist,0,t_dest_fmt,
		D3DPOOL_MANAGED,&t_dest
		));

	// Copy surfaces & destroy temporary
	ID3DTexture2D* T_src= t_from;
	ID3DTexture2D* T_dst= t_dest;

	int		L_src			= T_src->GetLevelCount	()-1;
	int		L_dst			= T_dst->GetLevelCount	()-1;
	for (; L_dst>=0; L_src--,L_dst--)
	{
		// Get surfaces
		IDirect3DSurface9		*S_src, *S_dst;
		R_CHK	(T_src->GetSurfaceLevel	(L_src,&S_src));
		R_CHK	(T_dst->GetSurfaceLevel	(L_dst,&S_dst));

		// Copy
		R_CHK	(D3DXLoadSurfaceFromSurface(S_dst,NULL,NULL,S_src,NULL,NULL,D3DX_FILTER_NONE,0));

		// Release surfaces
		_RELEASE				(S_src);
		_RELEASE				(S_dst);
	}

	// OK
	w						= top_width;
	h						= top_height;
	return					t_dest;
}

template	<class _It>
IC	void	TW_Iterate_1OP
(
 ID3DTexture2D*		t_dst,
 ID3DTexture2D*		t_src,
 const _It				pred
 )
{
	DWORD mips							= t_dst->GetLevelCount();
	R_ASSERT							(mips == t_src->GetLevelCount());
	for (DWORD i = 0; i < mips; i++)	{
		D3DLOCKED_RECT				Rsrc,Rdst;
		D3DSURFACE_DESC				desc,descS;

		t_dst->GetLevelDesc			(i, &desc);
		t_src->GetLevelDesc			(i, &descS);
		VERIFY						(desc.Format==descS.Format);
		VERIFY						(desc.Format==D3DFMT_A8R8G8B8);
		t_src->LockRect				(i,&Rsrc,0,0);
		t_dst->LockRect				(i,&Rdst,0,0);
		for (u32 y = 0; y < desc.Height; y++)	{
			for (u32 x = 0; x < desc.Width; x++)	{
				DWORD&	pSrc	= *(((DWORD*)((BYTE*)Rsrc.pBits + (y * Rsrc.Pitch)))+x);
				DWORD&	pDst	= *(((DWORD*)((BYTE*)Rdst.pBits + (y * Rdst.Pitch)))+x);
				pDst			= pred(pDst,pSrc);
			}
		}
		t_dst->UnlockRect			(i);
		t_src->UnlockRect			(i);
	}
}
template	<class _It>
IC	void	TW_Iterate_2OP
(
 ID3DTexture2D*		t_dst,
 ID3DTexture2D*		t_src0,
 ID3DTexture2D*		t_src1,
 const _It				pred
 )
{
	DWORD mips							= t_dst->GetLevelCount();
	R_ASSERT							(mips == t_src0->GetLevelCount());
	R_ASSERT							(mips == t_src1->GetLevelCount());
	for (DWORD i = 0; i < mips; i++)	{
		D3DLOCKED_RECT				Rsrc0,Rsrc1,Rdst;
		D3DSURFACE_DESC				desc,descS0,descS1;

		t_dst->GetLevelDesc			(i, &desc);
		t_src0->GetLevelDesc		(i, &descS0);
		t_src1->GetLevelDesc		(i, &descS1);
		VERIFY						(desc.Format==descS0.Format);
		VERIFY						(desc.Format==descS1.Format);
		VERIFY						(desc.Format==D3DFMT_A8R8G8B8);
		t_src0->LockRect			(i,&Rsrc0,	0,0);
		t_src1->LockRect			(i,&Rsrc1,	0,0);
		t_dst->LockRect				(i,&Rdst,	0,0);
		for (u32 y = 0; y < desc.Height; y++)	{
			for (u32 x = 0; x < desc.Width; x++)	{
				DWORD&	pSrc0	= *(((DWORD*)((BYTE*)Rsrc0.pBits + (y * Rsrc0.Pitch)))+x);
				DWORD&	pSrc1	= *(((DWORD*)((BYTE*)Rsrc1.pBits + (y * Rsrc1.Pitch)))+x);
				DWORD&	pDst	= *(((DWORD*)((BYTE*)Rdst.pBits  + (y * Rdst.Pitch)))+x);
				pDst			= pred(pDst,pSrc0,pSrc1);
			}
		}
		t_dst->UnlockRect			(i);
		t_src0->UnlockRect			(i);
		t_src1->UnlockRect			(i);
	}
}

IC u32 it_gloss_rev		(u32 d, u32 s)	{	return	color_rgba	(
	color_get_A(s),		// gloss
	color_get_B(d),
	color_get_G(d),
	color_get_R(d)		);
}
IC u32 it_gloss_rev_base(u32 d, u32 s)	{	
	u32		occ		= color_get_A(d)/3;
	u32		def		= 8;
	u32		gloss	= (occ*1+def*3)/4;
	return	color_rgba	(
		gloss,			// gloss
		color_get_B(d),
		color_get_G(d),
		color_get_R(d)
		);
}
IC u32 it_difference	(u32 d, u32 orig, u32 ucomp)	{	return	color_rgba(
	128+(int(color_get_R(orig))-int(color_get_R(ucomp)))*2,		// R-error
	128+(int(color_get_G(orig))-int(color_get_G(ucomp)))*2,		// G-error
	128+(int(color_get_B(orig))-int(color_get_B(ucomp)))*2,		// B-error
	128+(int(color_get_A(orig))-int(color_get_A(ucomp)))*2	);	// A-error	
}
IC u32 it_height_rev	(u32 d, u32 s)	{	return	color_rgba	(
	color_get_A(d),					// diff x
	color_get_B(d),					// diff y
	color_get_G(d),					// diff z
	color_get_R(s)	);				// height
}
IC u32 it_height_rev_base(u32 d, u32 s)	{	return	color_rgba	(
	color_get_A(d),					// diff x
	color_get_B(d),					// diff y
	color_get_G(d),					// diff z
	(color_get_R(s)+color_get_G(s)+color_get_B(s))/3	);	// height
}
*/
// Mip levels the last texture_load on this thread dropped (CTexture::m_mip_skip)
thread_local int g_texture_load_skip = 0;
// Texture streaming (r4_tex_stream): when set, 2D textures load with mips skipped until the largest is at most this
// size; g_texture_load_lod_normal is the skip they would have had (texture_lod)
thread_local int g_texture_load_max_dim = 0;
thread_local int g_texture_load_lod_normal = 0;

#if defined(USE_DX12)
#include "../xrRenderDX12/dx12DDS.h"

// The DX12 renderer loads DDS files itself (no D3DX): same file lookup and fallbacks as below, then dds12::load
ID3DBaseTexture* CRender::texture_load(LPCSTR fRName, u32& ret_msize, bool bStaging)
{
	g_texture_load_skip = 0;
	g_texture_load_lod_normal = 0;

	static bool bAllowStaging = !RImplementation.o.no_ram_textures;
	bStaging &= bAllowStaging;

	R_ASSERT(fRName);
	R_ASSERT(fRName[0]);

	string_path fn;
	string_path fname;
	xr_strcpy(fname, fRName);
	fix_texture_name(fname);

	// The file to load: the bump fallbacks of the DX11 path, then the level, saves, textures, the "missing" texture
	bool found = false;
	if (strstr(fname, "_bump"))
	{
		if (!FS.exist(fn, "$game_textures$", fname, ".dds"))
		{
			Msg("! Fallback to default bump map: %s", fname);
			R_ASSERT2(FS.exist(fn, "$game_textures$", strstr(fname, "_bump#") ? "ed\\ed_dummy_bump#" : "ed\\ed_dummy_bump", ".dds"), fname);
			found = true;
		}
		else if (Core.ParamsData.test(ECoreParams::no_bump_mode2) ||
		         (Core.ParamsData.test(ECoreParams::no_bump_mode1) && strstr(fname, "_bump#")))
		{
			R_ASSERT2(FS.exist(fn, "$game_textures$", strstr(fname, "_bump#") ? "ed\\ed_dummy_bump#" : "ed\\ed_dummy_bump", ".dds"), fname);
			found = true;
		}
	}
	if (!found)
		found = FS.exist(fn, "$level$", fname, ".dds") || FS.exist(fn, "$game_saves$", fname, ".dds") ||
			FS.exist(fn, "$game_textures$", fname, ".dds");
	if (!found)
	{
		Msg("! Can't find texture '%s'", fname);
		R_ASSERT(FS.exist(fn, "$game_textures$", "ed\\ed_not_existing_texture", ".dds"));
	}

	for (int attempt = 0; attempt < 2; ++attempt)
	{
		IReader* S = FS.r_open(fn);
		R_ASSERT2(S, fn);
		const u32 img_size = S->length();
		strlwr(fn);

		dds12::Info info;
		if (dds12::info(S->pointer(), S->length(), info))
		{
			int lod = get_texture_load_lod(fn);
			if (!info.cube && !info.volume)
			{
				g_texture_load_lod_normal = lod;
				if (g_texture_load_max_dim > 0)
				{
					int s = 0;
					while (s < 15 && ((info.width >> s) > u32(g_texture_load_max_dim) || (info.height >> s) > u32(g_texture_load_max_dim)))
						s++;
					if (s > lod)
						lod = s;
				}
			}
			u32 mip_cnt = 0;
			int skipped = 0;
			ID3DBaseTexture* T = dds12::load(S->pointer(), S->length(), lod, bStaging, mip_cnt, skipped);
			FS.r_close(S);
			if (T)
			{
				g_texture_load_skip = skipped;
				ret_msize = calc_texture_size(skipped, mip_cnt, img_size);
				return T;
			}
		}
		else
		{
			// Not a DDS file (a PNG named .dds, as D3DX accepted): one level, no texture_lod
			u32 w = 0, h = 0;
			ID3DBaseTexture* T = dds12::load_image(S->pointer(), S->length(), bStaging, w, h);
			FS.r_close(S);
			if (T)
			{
				Msg("* Texture '%s' is not a DDS file: loaded as an image (%ux%u RGBA8)", fn, w, h);
				ret_msize = w * h * 4;
				return T;
			}
		}
		Msg("! Can't load texture '%s' (unsupported DDS layout)", fn);
		if (!FS.exist(fn, "$game_textures$", "ed\\ed_not_existing_texture", ".dds"))
			break;
	}
	return 0;
}
#else
ID3DBaseTexture* CRender::texture_load(LPCSTR fRName, u32& ret_msize, bool bStaging)
{
	g_texture_load_skip = 0;
	g_texture_load_lod_normal = 0;
	//	Moved here just to avoid warning
#ifdef USE_DX11
	D3DX11_IMAGE_INFO IMG;
#else
	D3DX10_IMAGE_INFO IMG;
#endif
	ZeroMemory(&IMG, sizeof(IMG));

	//	Staging control
	static bool bAllowStaging = !RImplementation.o.no_ram_textures;
	bStaging &= bAllowStaging;

	ID3DBaseTexture* pTexture2D = NULL;
	//IDirect3DCubeTexture9*	pTextureCUBE	= NULL;
	string_path fn;
	//u32						dwWidth,dwHeight;
	u32 img_size = 0;
	int img_loaded_lod = 0;
	//D3DFORMAT				fmt;
	u32 mip_cnt = u32(-1);
	// validation
	R_ASSERT(fRName);
	R_ASSERT(fRName[0]);

	// make file name
	string_path fname;
	xr_strcpy(fname, fRName); //. andy if (strext(fname)) *strext(fname)=0;
	fix_texture_name(fname);
	IReader* S = NULL;
	//if (!FS.exist(fn,"$game_textures$",	fname,	".dds")	&& strstr(fname,"_bump"))	goto _BUMP_from_base;
	if (strstr(fname, "_bump"))
	{
		if (!FS.exist(fn, "$game_textures$", fname, ".dds"))
			goto _BUMP_from_base;
		else if (Core.ParamsData.test(ECoreParams::no_bump_mode2))
		{
			if (strstr(fname, "_bump#"))
			{
				R_ASSERT2(FS.exist(fn,"$game_textures$", "ed\\ed_dummy_bump#", ".dds"), "ed_dummy_bump#");
				S = FS.r_open(fn);
				R_ASSERT2(S, fn);
				img_size = S->length();
				goto _DDS_2D;
			}

			R_ASSERT2(FS.exist(fn,"$game_textures$", "ed\\ed_dummy_bump", ".dds"), "ed_dummy_bump");
			S = FS.r_open(fn);

			R_ASSERT2(S, fn);

			img_size = S->length();
			goto _DDS_2D;
		}
		else if (Core.ParamsData.test(ECoreParams::no_bump_mode1) && strstr(fname, "_bump#"))
		{
			R_ASSERT2(FS.exist(fn,"$game_textures$", "ed\\ed_dummy_bump#", ".dds"), "ed_dummy_bump#");
			S = FS.r_open(fn);
			R_ASSERT2(S, fn);
			img_size = S->length();
			goto _DDS_2D;
		}
	}
	if (FS.exist(fn, "$level$", fname, ".dds")) goto _DDS;
	if (FS.exist(fn, "$game_saves$", fname, ".dds")) goto _DDS;
	if (FS.exist(fn, "$game_textures$", fname, ".dds")) goto _DDS;


#ifdef _EDITOR
	ELog.Msg(mtError,"Can't find texture '%s'",fname);
	return 0;
#else

	Msg("! Can't find texture '%s'", fname);
	R_ASSERT(FS.exist(fn,"$game_textures$", "ed\\ed_not_existing_texture",".dds"));
	goto _DDS;

	//	Debug.fatal(DEBUG_INFO,"Can't find texture '%s'",fname);

#endif

_DDS:
	{
		// Load and get header

		S = FS.r_open(fn);
#ifdef DEBUG
		Msg						("* Loaded: %s[%d]",fn,S->length());
#endif // DEBUG
		img_size = S->length();
		R_ASSERT(S);
		//R_CHK2					(D3DXGetImageInfoFromFileInMemory	(S->pointer(),S->length(),&IMG), fn);
#ifdef USE_DX11
		R_CHK2(D3DX11GetImageInfoFromMemory(S->pointer(),S->length(), 0, &IMG, 0), fn);
#else
		R_CHK2(D3DX10GetImageInfoFromMemory(S->pointer(),S->length(), 0, &IMG, 0), fn);
#endif
		//if (IMG.ResourceType	== D3DRTYPE_CUBETEXTURE)			goto _DDS_CUBE;
		if (IMG.MiscFlags & D3D_RESOURCE_MISC_TEXTURECUBE) goto _DDS_CUBE;
		else goto _DDS_2D;

	_DDS_CUBE:
		{
			//R_CHK(D3DXCreateCubeTextureFromFileInMemoryEx(
			//	HW.pDevice,
			//	S->pointer(),S->length(),
			//	D3DX_DEFAULT,
			//	IMG.MipLevels,0,
			//	IMG.Format,
			//	D3DPOOL_MANAGED,
			//	D3DX_DEFAULT,
			//	D3DX_DEFAULT,
			//	0,&IMG,0,
			//	&pTextureCUBE
			//	));

#ifdef USE_DX11
			// Skip mip levels like 2D textures (the D3DX path below never could)
			{
				strlwr(fn);
				int skipped = 0;
				if (ID3DBaseTexture* T = dds_skip::load(S->pointer(), S->length(), get_texture_load_lod(fn), bStaging, mip_cnt, skipped))
				{
					g_texture_load_skip = skipped;
					FS.r_close(S);
					ret_msize = calc_texture_size(skipped, mip_cnt, img_size);
					return T;
				}
			}
#endif

			//	Inited to default by provided default constructor
#ifdef USE_DX11
			D3DX11_IMAGE_LOAD_INFO LoadInfo;
#else
			D3DX10_IMAGE_LOAD_INFO LoadInfo;
#endif
			//LoadInfo.Usage = D3D_USAGE_IMMUTABLE;
			if (bStaging)
			{
				LoadInfo.Usage = D3D_USAGE_STAGING;
				LoadInfo.BindFlags = 0;
				LoadInfo.CpuAccessFlags = D3D_CPU_ACCESS_WRITE;
			}
			else
			{
				LoadInfo.Usage = D3D_USAGE_IMMUTABLE;
				LoadInfo.BindFlags = D3D_BIND_SHADER_RESOURCE;
			}

			LoadInfo.pSrcInfo = &IMG;

#ifdef USE_DX11
			R_CHK(D3DX11CreateTextureFromMemory(
				HW.pDevice,
				S->pointer(),S->length(),
				&LoadInfo,
				0,
				&pTexture2D,
				0
			));
#else
			R_CHK(D3DX10CreateTextureFromMemory(
				HW.pDevice,
				S->pointer(),S->length(),
				&LoadInfo,
				0,
				&pTexture2D,
				0
			));
#endif

			FS.r_close(S);

			// OK
			mip_cnt = IMG.MipLevels;
			ret_msize = calc_texture_size(img_loaded_lod, mip_cnt, img_size);
			return pTexture2D;
		}
	_DDS_2D:
		{
			// Check for LMAP and compress if needed
			strlwr(fn);


			// Load   SYS-MEM-surface, bound to device restrictions
			//ID3DTexture2D*		T_sysmem;
			//R_CHK2(D3DXCreateTextureFromFileInMemoryEx
			//	(
			//	HW.pDevice,S->pointer(),S->length(),
			//	D3DX_DEFAULT,D3DX_DEFAULT,
			//	IMG.MipLevels,0,
			//	IMG.Format,
			//	D3DPOOL_SYSTEMMEM,
			//	D3DX_DEFAULT,
			//	D3DX_DEFAULT,
			//	0,&IMG,0,
			//	&T_sysmem
			//	), fn);

			img_loaded_lod = get_texture_load_lod(fn);

#ifdef USE_DX11
			g_texture_load_lod_normal = img_loaded_lod;
			if (g_texture_load_max_dim > 0)
			{
				int s = 0;
				while (s < 15 && ((IMG.Width >> s) > u32(g_texture_load_max_dim) || (IMG.Height >> s) > u32(g_texture_load_max_dim)))
					s++;
				if (s > img_loaded_lod)
					img_loaded_lod = s;
			}
			{
				int skipped = 0;
				if (ID3DBaseTexture* T = dds_skip::load(S->pointer(), S->length(), img_loaded_lod, bStaging, mip_cnt, skipped))
				{
					g_texture_load_skip = skipped;
					FS.r_close(S);
					ret_msize = calc_texture_size(skipped, mip_cnt, img_size);
					return T;
				}
			}

			// Not handled above: load it whole. D3DX11 never applied the skip anyway, and asking for one sends it
			// down its conversion path (CPU resize and re-encode), suspected for GAMMA's load screen freezes with
			// texture_lod 2+
			img_loaded_lod = 0;
#endif

			//	Inited to default by provided default constructor
#ifdef USE_DX11
			D3DX11_IMAGE_LOAD_INFO LoadInfo;
#else
			D3DX10_IMAGE_LOAD_INFO LoadInfo;
#endif
			LoadInfo.FirstMipLevel = img_loaded_lod;
			LoadInfo.MipLevels = IMG.MipLevels;
			LoadInfo.Width = IMG.Width;
			LoadInfo.Height = IMG.Height;

			if (img_loaded_lod)
			{
				Reduce(LoadInfo.Width, LoadInfo.Height, IMG.MipLevels, img_loaded_lod);
			}

			//LoadInfo.Usage = D3D_USAGE_IMMUTABLE;
			if (bStaging)
			{
				LoadInfo.Usage = D3D_USAGE_STAGING;
				LoadInfo.BindFlags = 0;
				LoadInfo.CpuAccessFlags = D3D_CPU_ACCESS_WRITE;
			}
			else
			{
				LoadInfo.Usage = D3D_USAGE_IMMUTABLE;
				LoadInfo.BindFlags = D3D_BIND_SHADER_RESOURCE;
			}
			LoadInfo.pSrcInfo = &IMG;

         HRESULT textureLoadResult;
#ifdef USE_DX11
            textureLoadResult = D3DX11CreateTextureFromMemory
								(
									HW.pDevice,S->pointer(),S->length(),
									&LoadInfo,
									0,
									&pTexture2D,
									0
								);
#else
            textureLoadResult = D3DX10CreateTextureFromMemory
								(
									HW.pDevice,S->pointer(),S->length(),
									&LoadInfo,
									0,
									&pTexture2D,
									0
									);
#endif
          if (FAILED(textureLoadResult))
			{
				HRESULT deviceRemovedReason = HW.pDevice->GetDeviceRemovedReason();
				Msg("! Texture load HRESULT: 0x%08X; GetDeviceRemovedReason: 0x%08X", (u32)textureLoadResult, (u32)deviceRemovedReason);
			}
			R_CHK2(textureLoadResult, fn);
			FS.r_close(S);
			mip_cnt = IMG.MipLevels;
			// OK
			ret_msize = calc_texture_size(img_loaded_lod, mip_cnt, img_size);
			return pTexture2D;
		}
	}

_BUMP_from_base:
	{
		//Msg			("! auto-generated bump map: %s",fname);
		Msg("! Fallback to default bump map: %s", fname);
		//////////////////
		if (strstr(fname, "_bump#"))
		{
			R_ASSERT2(FS.exist(fn,"$game_textures$", "ed\\ed_dummy_bump#", ".dds"), "ed_dummy_bump#");
			S = FS.r_open(fn);
			R_ASSERT2(S, fn);
			img_size = S->length();
			goto _DDS_2D;
		}
		if (strstr(fname, "_bump"))
		{
			R_ASSERT2(FS.exist(fn,"$game_textures$", "ed\\ed_dummy_bump", ".dds"), "ed_dummy_bump");
			S = FS.r_open(fn);

			R_ASSERT2(S, fn);

			img_size = S->length();
			goto _DDS_2D;
		}
		//////////////////
	}

	return 0;
}
#endif // USE_DX12
