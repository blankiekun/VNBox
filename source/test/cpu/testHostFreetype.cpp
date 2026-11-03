/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "boxedwine.h"

#if defined(__TEST) && defined(BOXEDWINE_HOST_FREETYPE)

// Host FreeType (hostfreetype.cpp): the calls win32u makes on a font (FT_New_Memory_Face on the guest's copy of the
// file, FT_Set_Pixel_Sizes, FT_Set_Charmap, FT_Load_Glyph with its GGO load flags, then FT_Outline_Translate as guest
// code on the mirrored outline and FT_Outline_Get_Bitmap / FT_Render_Glyph) must leave in guest memory what host
// FreeType gives for the same calls on its own face: face fields, size metrics, glyph metrics, outline arrays, bitmaps.
// Catches a mirror field at the wrong offset, a pointer left pointing at host memory, a slot not synced after a call,
// a render that ignores the guest's moved points, and calls on objects the host doesn't own being taken.
// That host FreeType 2.11.1 gives the root fs libfreetype.so.6's bytes is checked outside (lib/freetype/README.md).
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_LCD_FILTER_H
#include FT_TRUETYPE_TABLES_H
#include "testHostFreetype.h"
#include "testCPU.h"

U32 hostFreetypeFnId(const char* name);
bool hostFreetypeRun(CPU* cpu, U32 fn);
extern U32 hostFreetypeTestCaller;

#define cpu (testContext().cpu)
#define memory (testContext().memory)

namespace {

constexpr U32 RET = 0x12345678, GUEST_LIB = 0x4c1b0000;
U32 heap, stack, font, aface, bitmapBuf, bitmapRec, outlineRec;

// fn(args) as the guest's cdecl call arrives; true when done on the host (then EAX is the result).
bool call(const char* name, std::initializer_list<U32> args) {
    U32 fn = hostFreetypeFnId(name);
    if (!fn) testFail("%s not hooked", name);
    ESP = stack;
    memory->writed(stack, RET);
    U32 a = stack + 4;
    for (U32 v : args) { memory->writed(a, v); a += 4; }
    EAX = 0xDEADBEEF;
    cpu->eip.u32 = 0x1000;
    bool ran = hostFreetypeRun(cpu, fn - 1);
    if (ran && (ESP != stack + 4 || cpu->eip.u32 != RET)) testFail("%s did not return like `ret`", name);
    if (!ran && (ESP != stack || cpu->eip.u32 != 0x1000 || EAX != 0xDEADBEEF)) testFail("declined %s changed registers", name);
    return ran;
}

template <typename T> T guest(U32 address) {
    T v;
    memory->memcpy(&v, address, sizeof(T));
    return v;
}
U32 gaddr(const void* p) { return (U32)(uintptr_t)p; } // a guest pointer inside a guest struct

void expect(bool ok, const char* what, U32 glyph, int ppem, int flags) {
    if (!ok) testFail("%s differs: glyph %u ppem %d flags %x", what, glyph, ppem, flags);
}

// The guest outline at `o` moved by (dx, dy), as FT_Outline_Translate running as guest code does it.
void guestTranslate(U32 o, FT_Pos dx, FT_Pos dy) {
    FT_Outline g = guest<FT_Outline>(o);
    for (int i = 0; i < g.n_points; i++) {
        U32 p = gaddr(g.points) + i * sizeof(FT_Vector);
        memory->writed(p, memory->readd(p) + dx);
        memory->writed(p + 4, memory->readd(p + 4) + dy);
    }
}

}

void testHostFreetypeGlyphs() {
    testNewInstruction(0);
    for (int s : { CS, DS, ES, SS }) cpu->seg[s].address = 0;
    std::vector<U8> data;
    FILE* f = fopen("/hostft/Cousine-Regular.ttf", "rb");
    if (!f) testFail("no test font");
    for (int c; (c = fgetc(f)) != EOF;) data.push_back((U8)c);
    fclose(f);
    heap = memory->mmap(cpu->thread, 0, 0x200000, K_PROT_READ | K_PROT_WRITE, K_MAP_PRIVATE | K_MAP_ANONYMOUS, -1, 0);
    stack = heap + 0x1000;
    aface = heap + 0x2000;
    outlineRec = heap + 0x2100;
    bitmapRec = heap + 0x2200;
    bitmapBuf = heap + 0x10000;
    font = heap + 0x100000;
    memory->memcpy(font, data.data(), (U32)data.size());

    FT_Library lib;
    FT_Face want;
    FT_Init_FreeType(&lib);
    FT_Library_SetLcdFilter(lib, FT_LCD_FILTER_DEFAULT);
    if (FT_New_Memory_Face(lib, data.data(), (FT_Long)data.size(), 0, &want)) testFail("host FreeType can't open the test font");

    // Not from win32u: left to the guest (fontconfig and dwrite call more FT_* functions than are hooked).
    hostFreetypeTestCaller = 0;
    if (call("FT_New_Memory_Face", { GUEST_LIB, font, (U32)data.size(), 0, aface })) testFail("FT_New_Memory_Face from another module taken");
    hostFreetypeTestCaller = RET;
    if (call("FT_Library_SetLcdFilter", { GUEST_LIB, FT_LCD_FILTER_DEFAULT })) testFail("FT_Library_SetLcdFilter not left to the guest too");
    if (!call("FT_New_Memory_Face", { GUEST_LIB, font, (U32)data.size(), 0, aface }) || EAX) testFail("FT_New_Memory_Face not done on the host");
    U32 face = memory->readd(aface);
    FT_FaceRec fr = guest<FT_FaceRec>(face);
    if (fr.num_glyphs != want->num_glyphs || fr.units_per_EM != want->units_per_EM || fr.face_flags != want->face_flags ||
        fr.num_charmaps != want->num_charmaps || memcmp(&fr.bbox, &want->bbox, sizeof(fr.bbox)) || fr.ascender != want->ascender)
        testFail("face fields differ");
    if (memory->readString(gaddr(fr.family_name)) != want->family_name || memory->readString(gaddr(fr.style_name)) != want->style_name)
        testFail("face names differ");
    for (int i = 0; i < fr.num_charmaps; i++) {
        FT_CharMapRec c = guest<FT_CharMapRec>(memory->readd(gaddr(fr.charmaps) + i * 4));
        if (c.encoding != want->charmaps[i]->encoding || c.platform_id != want->charmaps[i]->platform_id || gaddr(c.face) != face)
            testFail("charmap %d differs", i);
    }
    TT_OS2* os2 = (TT_OS2*)FT_Get_Sfnt_Table(want, FT_SFNT_OS2);
    if (!call("FT_Get_Sfnt_Table", { face, FT_SFNT_OS2 }) || !EAX) testFail("no OS/2 table");
    TT_OS2 gos2 = guest<TT_OS2>(EAX);
    if (memcmp(&gos2, os2, sizeof(gos2))) testFail("OS/2 table differs");
    // a charmap pointer of the mirror selects that charmap
    U32 last = memory->readd(gaddr(fr.charmaps) + (fr.num_charmaps - 1) * 4);
    if (!call("FT_Set_Charmap", { face, last }) || EAX || guest<FT_FaceRec>(face).charmap != (FT_CharMap)(uintptr_t)last) testFail("FT_Set_Charmap");
    FT_Set_Charmap(want, want->charmaps[want->num_charmaps - 1]);
    if (!call("FT_Get_Char_Index", { face, 'g' }) || EAX != FT_Get_Char_Index(want, 'g')) testFail("FT_Get_Char_Index");

    // GGO_BITMAP, GGO_GRAY*_BITMAP, WINE_GGO_HRGB_BITMAP, GGO_UNHINTED (freetype.c's get_load_flags)
    const FT_Int32 loads[] = { FT_LOAD_TARGET_MONO, FT_LOAD_TARGET_NORMAL | FT_LOAD_NO_BITMAP, FT_LOAD_TARGET_LCD | FT_LOAD_NO_BITMAP, FT_LOAD_NO_HINTING };
    U32 glyphs = 0;
    for (int ppem : { 9, 13, 24 }) {
        if (!call("FT_Set_Pixel_Sizes", { face, 0, (U32)ppem }) || EAX) testFail("FT_Set_Pixel_Sizes");
        FT_Set_Pixel_Sizes(want, 0, ppem);
        FT_SizeRec sr = guest<FT_SizeRec>(gaddr(guest<FT_FaceRec>(face).size));
        if (memcmp(&sr.metrics, &want->size->metrics, sizeof(sr.metrics))) testFail("size metrics differ at %d ppem", ppem);
        for (int li = 0; li < 4; li++) {
            FT_Int32 flags = loads[li] | FT_LOAD_IGNORE_GLOBAL_ADVANCE_WIDTH | (loads[li] & FT_LOAD_NO_HINTING ? 0 : FT_LOAD_PEDANTIC);
            for (U32 g = 0; g < (U32)want->num_glyphs; g++) {
                FT_Error err = FT_Load_Glyph(want, g, flags);
                if (!call("FT_Load_Glyph", { face, g, (U32)flags }) || EAX != (U32)err) testFail("FT_Load_Glyph result, glyph %u", g);
                if (err) continue;
                glyphs++;
                FT_GlyphSlot ws = want->glyph;
                U32 slot = gaddr(guest<FT_FaceRec>(face).glyph);
                FT_GlyphSlotRec gs = guest<FT_GlyphSlotRec>(slot);
                expect(!memcmp(&gs.metrics, &ws->metrics, sizeof(gs.metrics)) && gs.advance.x == ws->advance.x && gs.format == ws->format &&
                    gs.linearHoriAdvance == ws->linearHoriAdvance && gs.lsb_delta == ws->lsb_delta, "glyph metrics", g, ppem, flags);
                FT_Outline& wo = ws->outline;
                expect(gs.outline.n_points == wo.n_points && gs.outline.n_contours == wo.n_contours && gs.outline.flags == wo.flags, "outline size", g, ppem, flags);
                if (wo.n_points) {
                    expect(!memory->memcmp(gaddr(gs.outline.points), wo.points, wo.n_points * sizeof(FT_Vector)) &&
                        !memory->memcmp(gaddr(gs.outline.tags), wo.tags, wo.n_points) &&
                        !memory->memcmp(gaddr(gs.outline.contours), wo.contours, wo.n_contours * 2), "outline", g, ppem, flags);
                }
                if (ws->format != FT_GLYPH_FORMAT_OUTLINE || !wo.n_points) continue;
                FT_BBox bb;
                FT_Outline_Get_CBox(&wo, &bb);
                bb.xMin &= ~63; bb.yMin &= ~63; bb.xMax = (bb.xMax + 63) & ~63; bb.yMax = (bb.yMax + 63) & ~63;
                if (li == 2) { // LCD: FT_Render_Glyph on the slot after the guest moved the points
                    FT_Outline_Translate(&wo, 64 * 3 + 17, -40);
                    guestTranslate(slot + offsetof(FT_GlyphSlotRec, outline), 64 * 3 + 17, -40);
                    FT_Error rerr = FT_Render_Glyph(ws, FT_RENDER_MODE_LCD);
                    if (!call("FT_Render_Glyph", { slot, FT_RENDER_MODE_LCD }) || EAX != (U32)rerr) testFail("FT_Render_Glyph result");
                    gs = guest<FT_GlyphSlotRec>(slot);
                    U32 len = ws->bitmap.rows * (U32)std::abs(ws->bitmap.pitch);
                    expect(gs.bitmap.rows == ws->bitmap.rows && gs.bitmap.width == ws->bitmap.width && gs.bitmap.pitch == ws->bitmap.pitch &&
                        gs.bitmap_left == ws->bitmap_left && gs.bitmap_top == ws->bitmap_top && gs.format == ws->format &&
                        (!len || !memory->memcmp(gaddr(gs.bitmap.buffer), ws->bitmap.buffer, len)), "LCD render", g, ppem, flags);
                    continue;
                }
                // GGO's path: translate to the bitmap origin (guest code), FT_Outline_Get_Bitmap into a guest buffer
                int w = (bb.xMax - bb.xMin) >> 6, h = (bb.yMax - bb.yMin) >> 6;
                if (w <= 0 || h <= 0 || w > 400 || h > 400) continue;
                FT_Outline_Translate(&wo, -bb.xMin, -bb.yMin);
                guestTranslate(slot + offsetof(FT_GlyphSlotRec, outline), -bb.xMin, -bb.yMin);
                static U8 pixels[400 * 400];
                FT_Bitmap b = {};
                b.width = w;
                b.rows = h;
                b.pitch = li == 0 ? (w + 31) / 32 * 4 : (w + 3) & ~3;
                b.pixel_mode = li == 0 ? FT_PIXEL_MODE_MONO : FT_PIXEL_MODE_GRAY;
                b.num_grays = 256;
                ::memset(pixels, 0, b.pitch * h);
                memory->memset(bitmapBuf, 0, b.pitch * h);
                b.buffer = pixels;
                FT_Error berr = FT_Outline_Get_Bitmap(lib, &wo, &b);
                b.buffer = (unsigned char*)(uintptr_t)bitmapBuf;
                memory->memcpy(bitmapRec, &b, sizeof(b));
                if (!call("FT_Outline_Get_Bitmap", { GUEST_LIB, slot + (U32)offsetof(FT_GlyphSlotRec, outline), bitmapRec }) || EAX != (U32)berr)
                    testFail("FT_Outline_Get_Bitmap result");
                expect(!memory->memcmp(bitmapBuf, pixels, b.pitch * h), "outline bitmap", g, ppem, flags);
            }
        }
    }
    if (glyphs < 1000) testFail("only %u glyphs compared", glyphs);

    // objects the host didn't create stay guest code
    if (call("FT_Load_Glyph", { heap + 0x3000, 1, 0 }) || call("FT_Render_Glyph", { heap + 0x3000, 0 }) || call("FT_Done_Face", { heap + 0x3000 }))
        testFail("call on a guest face taken");
    if (!call("FT_Done_Face", { face }) || EAX) testFail("FT_Done_Face");
    if (call("FT_Load_Glyph", { face, 1, 0 })) testFail("call on a freed face taken");
    FT_Done_Face(want);
    FT_Done_FreeType(lib);
    hostFreetypeTestCaller = 0;
}

#endif
