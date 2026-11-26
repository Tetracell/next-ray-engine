#include "stdafx.h"
#include "dxFontRender.h"

#include "../../xrEngine/GameFont.h"
#include "../../xrEngine/gamepad_glyphs.h"

dxFontRender::dxFontRender()
{
}

dxFontRender::~dxFontRender()
{
	pShader.destroy();
	pGlyphShader.destroy();
	pGeom.destroy();
}

void dxFontRender::Initialize(LPCSTR cShader, LPCSTR cTexture)
{
	pShader.create(cShader, cTexture);
	pGlyphShader.create(cShader, "ui\\ui_gamepad_glyphs");
	pGeom.create(FVF::F_TL, RCache.Vertex.Buffer(), RCache.QuadIB);
}

extern ENGINE_API xr_atomic_bool g_bRendering;
extern ENGINE_API Fvector2 g_current_font_scale;

namespace
{
	// A gamepad glyph to draw after the text of its batch: its square on the screen
	struct glyph_quad
	{
		float x, y, size;
		int glyph;
		u32 color;
	};
}

void dxFontRender::OnRender(CGameFont& owner)
{
	VERIFY(g_bRendering);
	if (pShader) RCache.set_Shader(pShader);

	if (!(owner.uFlags & CGameFont::fsValid))
	{
		CTexture* T = RCache.get_ActiveTexture(0);
		owner.vTS.set((int)T->get_Width(), (int)T->get_Height());
		owner.fTCHeight = owner.fHeight / float(owner.vTS.y);
		owner.uFlags |= CGameFont::fsValid;
	}

	for (u32 i = 0; i < owner.strings.size();)
	{
		// calculate first-fit
		int count = 1;

		int length = owner.smart_strlen(owner.strings[i].string);

		while ((i + count) < owner.strings.size())
		{
			int L = owner.smart_strlen(owner.strings[i + count].string);

			if ((L + length) < MAX_MB_CHARS)
			{
				count ++;
				length += L;
			}
			else break;
		}

		// lock AGP memory
		u32 vOffset;
		FVF::TL* v = (FVF::TL*)RCache.Vertex.Lock(length * 4, pGeom.stride(), vOffset);
		FVF::TL* start = v;

		glyph_quad glyphs[64];
		u32 glyph_count = 0;

		// fill vertices
		u32 last = i + count;
		for (; i < last; i++)
		{
			CGameFont::String& PS = owner.strings[i];
			wide_char wsStr[ MAX_MB_CHARS ];

			int len = owner.IsMultibyte() ? mbhMulti2Wide(wsStr, NULL, MAX_MB_CHARS, PS.string) : xr_strlen(PS.string);

			if (len)
			{
				float X = float(iFloor(PS.x));
				float Y = float(iFloor(PS.y));
				float S = PS.height * g_current_font_scale.y;
				float Y2 = Y + S;
				float fSize = 0;

				const float hs = fis_zero(owner.fHeight) ? 1.0f : (PS.height / owner.fHeight);

				if (PS.align)
				{
					fSize = owner.IsMultibyte() ? owner.SizeOf_(wsStr) : owner.SizeOf_(PS.string);
					if (!fis_zero(owner.GetHeight()))
						fSize *= PS.height / owner.GetHeight();
				}

				switch (PS.align)
				{
				case CGameFont::alCenter:
					X -= (iFloor(fSize * 0.5f)) * g_current_font_scale.x;
					break;
				case CGameFont::alRight:
					X -= iFloor(fSize);
					break;
				}

				u32 clr, clr2;
				clr2 = clr = PS.c;
				if (owner.uFlags & CGameFont::fsGradient)
				{
					u32 _R = color_get_R(clr) / 2;
					u32 _G = color_get_G(clr) / 2;
					u32 _B = color_get_B(clr) / 2;
					u32 _A = color_get_A(clr);
					clr2 = color_rgba(_R, _G, _B, _A);
				}

#if defined(USE_DX10) || defined(USE_DX11)		//	Vertex shader will cancel a DX9 correction, so make fake offset
				X -= 0.5f;
				Y -= 0.5f;
				Y2 -= 0.5f;
#endif	//	USE_DX10

				float tu, tv;
				for (int j = 0; j < len; j++)
				{
					Fvector l;

					l = owner.IsMultibyte() ? owner.GetCharTC(wsStr[1 + j]) : owner.GetCharTC((u16)(u8)PS.string[j]);

					float scw = l.z * hs * g_current_font_scale.x;

					float fTCWidth = l.z / owner.vTS.x;

					if (l.x < 0.f)
					{
						// A gamepad glyph: a square of the text's height in the middle of its width
						const int g = owner.IsMultibyte() ? -1 : gamepad_glyphs::from_char((u8)PS.string[j]);
						if (g >= 0 && glyph_count < sizeof(glyphs) / sizeof(glyphs[0]))
						{
							const float size = _min(S, scw / 1.2f);
							glyphs[glyph_count++] = {X + (scw - size) * 0.5f, Y + (S - size) * 0.5f, size, g,
								color_rgba(255, 255, 255, color_get_A(clr))};
						}
					}
					else if (!fis_zero(l.z))
					{
						//						tu			= ( l.x / owner.vTS.x ) + ( 0.5f / owner.vTS.x );
						//						tv			= ( l.y / owner.vTS.y ) + ( 0.5f / owner.vTS.y );
						tu = (l.x / owner.vTS.x);
						tv = (l.y / owner.vTS.y);
#if !defined(USE_DX10) && !defined(USE_DX11)
						//	Make half pixel offset for 1 to 1 mapping
						tu += (0.5f / owner.vTS.x);
						tv += (0.5f / owner.vTS.y);
#endif

						v->set(X, Y2, clr2, tu, tv + owner.fTCHeight);
						v++;
						v->set(X, Y, clr, tu, tv);
						v++;
						v->set(X + scw, Y2, clr2, tu + fTCWidth, tv + owner.fTCHeight);
						v++;
						v->set(X + scw, Y, clr, tu + fTCWidth, tv);
						v++;
					}
					X += scw * owner.vInterval.x;
					if (owner.IsMultibyte())
					{
						X -= 2;
						if (IsNeedSpaceCharacter(wsStr[1 + j]))
							X += owner.fXStep;
					}
				}
			}
		}

		// Unlock and draw
		u32 vCount = (u32)(v - start);
		RCache.Vertex.Unlock(vCount, pGeom.stride());
		if (vCount)
		{
			RCache.set_Geometry(pGeom);
			RCache.Render(D3DPT_TRIANGLELIST, vOffset, 0, vCount, 0, vCount / 2);
		}

		// The glyphs: cells of 64 in a 512 x 512 atlas, the PlayStation set from cell 32
		if (glyph_count && pGlyphShader)
		{
			FVF::TL* g = (FVF::TL*)RCache.Vertex.Lock(glyph_count * 4, pGeom.stride(), vOffset);
			FVF::TL* g0 = g;
			for (u32 k = 0; k < glyph_count; ++k)
			{
				const glyph_quad& q = glyphs[k];
				const int cell = q.glyph + (gamepad_glyphs::playstation ? 32 : 0);
				const float tu = float(cell % 8) / 8.f, tv = float(cell / 8) / 8.f, t = 1.f / 8.f;
				g->set(q.x, q.y + q.size, q.color, tu, tv + t);
				g++;
				g->set(q.x, q.y, q.color, tu, tv);
				g++;
				g->set(q.x + q.size, q.y + q.size, q.color, tu + t, tv + t);
				g++;
				g->set(q.x + q.size, q.y, q.color, tu + t, tv);
				g++;
			}
			const u32 gCount = (u32)(g - g0);
			RCache.Vertex.Unlock(gCount, pGeom.stride());
			RCache.set_Shader(pGlyphShader);
			RCache.set_Geometry(pGeom);
			RCache.Render(D3DPT_TRIANGLELIST, vOffset, 0, gCount, 0, gCount / 2);
			if (pShader)
				RCache.set_Shader(pShader);
		}
	}
}
