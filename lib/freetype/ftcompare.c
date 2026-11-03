// ftcompare <libfreetype.so> <font>...: hashes what win32u gets from FreeType (face fields, OS/2 table, size metrics,
// glyph metrics, outlines, gray/mono FT_Outline_Get_Bitmap, LCD FT_Render_Glyph) per font, face, ppem and load flags,
// for every glyph (STEP=n: every nth). Two FreeType builds that print the same lines give win32u the same bytes.
// VERBOSE=1 prints a hash per glyph; GLYPH=g FL=f PPEM=p details one glyph. See README.md for the builds compared.
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_LCD_FILTER_H
#include FT_TRUETYPE_TABLES_H
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define F(n) static __typeof__(n)* p##n;
F(FT_Init_FreeType) F(FT_New_Memory_Face) F(FT_Done_Face) F(FT_Set_Pixel_Sizes) F(FT_Load_Glyph) F(FT_Outline_Get_Bitmap)
F(FT_Render_Glyph) F(FT_Library_SetLcdFilter) F(FT_Outline_Translate) F(FT_Outline_Get_CBox) F(FT_Get_Sfnt_Table) F(FT_Library_Version)
F(FT_Get_Char_Index)
static unsigned h;
static void mix(const void* p, size_t n) { const unsigned char* c = p; while (n--) h = (h ^ *c++) * 16777619u; }
static int dbg;
#define MIX(v) do { mix(&(v), sizeof(v)); if (dbg) printf("  %s %08x\n", #v, h); } while (0)
static unsigned char buf[1 << 22];
int main(int argc, char** argv) {
#ifdef FTDUMP_STATIC
#define L(n) p##n = n;
#else
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "%s\n", dlerror()); return 1; }
#define L(n) p##n = dlsym(lib, #n);
#endif
    L(FT_Init_FreeType) L(FT_New_Memory_Face) L(FT_Done_Face) L(FT_Set_Pixel_Sizes) L(FT_Load_Glyph) L(FT_Outline_Get_Bitmap)
    L(FT_Render_Glyph) L(FT_Library_SetLcdFilter) L(FT_Outline_Translate) L(FT_Outline_Get_CBox) L(FT_Get_Sfnt_Table) L(FT_Library_Version)
    L(FT_Get_Char_Index)
    FT_Library library; pFT_Init_FreeType(&library);
    FT_Int ma, mi, pa; pFT_Library_Version(library, &ma, &mi, &pa);
    printf("version %d.%d.%d lcdfilter=%d\n", ma, mi, pa, pFT_Library_SetLcdFilter(library, FT_LCD_FILTER_DEFAULT));
    int step = getenv("STEP") ? atoi(getenv("STEP")) : 1;
    static const int ppems[] = { 8, 11, 12, 13, 16, 19, 24, 32, 48 };
    static const FT_Int32 flags[] = { FT_LOAD_TARGET_MONO, FT_LOAD_TARGET_NORMAL, FT_LOAD_TARGET_LCD, FT_LOAD_NO_HINTING, FT_LOAD_TARGET_MONO | FT_LOAD_NO_BITMAP };
    for (int a = 2; a < argc; a++) {
        FILE* f = fopen(argv[a], "rb"); fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
        unsigned char* data = malloc(size); fread(data, 1, size, f); fclose(f);
        for (int fi = 0, nf = 1; fi < nf; fi++) {
            FT_Face face;
            if (pFT_New_Memory_Face(library, data, size, fi, &face)) { printf("%s:%d open failed\n", argv[a], fi); break; }
            nf = face->num_faces;
            h = 2166136261u;
            MIX(face->num_glyphs); MIX(face->face_flags); MIX(face->style_flags); MIX(face->units_per_EM); MIX(face->bbox);
            MIX(face->ascender); MIX(face->descender); MIX(face->height); MIX(face->num_charmaps); MIX(face->num_fixed_sizes);
            mix(face->family_name, strlen(face->family_name)); mix(face->style_name, strlen(face->style_name));
            TT_OS2* os2 = pFT_Get_Sfnt_Table(face, FT_SFNT_OS2); if (os2) MIX(*os2);
            printf("%s:%d face %08x glyphs=%ld\n", argv[a], fi, h, face->num_glyphs);
            for (int s = 0; s < (int)(sizeof(ppems) / 4); s++) {
                if (FT_IS_SCALABLE(face)) pFT_Set_Pixel_Sizes(face, 0, ppems[s]);
                else if (s) break;
                for (int fl = 0; fl < 5; fl++) {
                    h = 2166136261u;
                    MIX(face->size->metrics);
                    int fails = 0;
                    for (long g = 0; g < face->num_glyphs; g += step) {
                        if (getenv("VERBOSE")) printf("g %ld fl=%d %08x\n", g, fl, h);
                        dbg = getenv("GLYPH") && atol(getenv("GLYPH")) == g && fl == atoi(getenv("FL")) && ppems[s] == atoi(getenv("PPEM"));
                        FT_Int32 lf = flags[fl] | FT_LOAD_IGNORE_GLOBAL_ADVANCE_WIDTH | (flags[fl] & FT_LOAD_NO_HINTING ? 0 : FT_LOAD_PEDANTIC);
                        FT_Error e = pFT_Load_Glyph(face, g, lf);
                        MIX(e);
                        if (e) { fails++; continue; }
                        FT_GlyphSlot gs = face->glyph;
                        MIX(gs->metrics); MIX(gs->advance); MIX(gs->linearHoriAdvance); MIX(gs->format); MIX(gs->lsb_delta); MIX(gs->rsb_delta);
                        if (gs->format == FT_GLYPH_FORMAT_BITMAP) {
                            MIX(gs->bitmap.rows); MIX(gs->bitmap.width); MIX(gs->bitmap.pitch); MIX(gs->bitmap_left); MIX(gs->bitmap_top);
                            mix(gs->bitmap.buffer, gs->bitmap.rows * abs(gs->bitmap.pitch));
                            continue;
                        }
                        if (gs->format != FT_GLYPH_FORMAT_OUTLINE) continue;
                        FT_Outline* o = &gs->outline;
                        MIX(o->n_points); MIX(o->n_contours); MIX(o->flags);
                        mix(o->points, o->n_points * sizeof(FT_Vector)); mix(o->tags, o->n_points); mix(o->contours, o->n_contours * 2);
                        FT_BBox bb; pFT_Outline_Get_CBox(o, &bb);
                        bb.xMin &= ~63; bb.yMin &= ~63; bb.xMax = (bb.xMax + 63) & ~63; bb.yMax = (bb.yMax + 63) & ~63;
                        int w = (bb.xMax - bb.xMin) >> 6, hh = (bb.yMax - bb.yMin) >> 6;
                        if (w <= 0 || hh <= 0 || w > 2000 || hh > 2000) continue;
                        FT_Vector save[o->n_points ? o->n_points : 1]; memcpy(save, o->points, o->n_points * sizeof(FT_Vector));
                        pFT_Outline_Translate(o, -bb.xMin, -bb.yMin);
                        FT_Bitmap bm; memset(&bm, 0, sizeof(bm));
                        bm.width = w; bm.rows = hh; bm.buffer = buf;
                        if (fl == 0 || fl == 4) { bm.pitch = (w + 31) / 32 * 4; bm.pixel_mode = FT_PIXEL_MODE_MONO; }
                        else { bm.pitch = (w + 3) & ~3; bm.pixel_mode = FT_PIXEL_MODE_GRAY; bm.num_grays = 256; }
                        memset(buf, 0, bm.pitch * hh);
                        e = pFT_Outline_Get_Bitmap(library, o, &bm); MIX(e); mix(buf, bm.pitch * hh); if (dbg) printf("  bitmap %08x\n", h);
                        memcpy(o->points, save, o->n_points * sizeof(FT_Vector));
                        if (fl == 2) {
                            e = pFT_Render_Glyph(gs, FT_RENDER_MODE_LCD); MIX(e);
                            MIX(gs->bitmap.rows); MIX(gs->bitmap.width); MIX(gs->bitmap.pitch); MIX(gs->bitmap_left); MIX(gs->bitmap_top);
                            if (!e) mix(gs->bitmap.buffer, gs->bitmap.rows * abs(gs->bitmap.pitch));
                            if (dbg) { printf("  lcd %08x %dx%d\n", h, gs->bitmap.width, gs->bitmap.rows); for (unsigned r = 0; r < gs->bitmap.rows; r++) { for (unsigned c = 0; c < gs->bitmap.width; c++) printf("%02x", gs->bitmap.buffer[r * gs->bitmap.pitch + c]); printf("\n"); } }
                        }
                    }
                    printf("%s:%d ppem=%d flags=%d %08x fails=%d\n", argv[a], fi, ppems[s], fl, h, fails);
                }
            }
            pFT_Done_Face(face);
        }
        free(data);
    }
    return 0;
}
