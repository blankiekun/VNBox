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
// file, FT_Set_Pixel_Sizes, FT_Set_Charmap, FT_Load_Glyph with its GGO load flags, FT_Outline_EmboldenXY for a fake
// bold font, then FT_Outline_Translate as guest code on the mirrored outline and FT_Outline_Get_Bitmap / FT_Render_Glyph) must leave in guest memory what host
// FreeType gives for the same calls on its own face: face fields, size metrics, glyph metrics, outline arrays, bitmaps.
// Catches a mirror field at the wrong offset, a pointer left pointing at host memory, a slot not synced after a call,
// a render that ignores the guest's moved points, and calls on objects the host doesn't own being taken.
// That host FreeType 2.11.1 gives the root fs libfreetype.so.6's bytes is checked outside (lib/freetype/README.md).
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_LCD_FILTER_H
#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_TAGS_H
#include FT_SFNT_NAMES_H
#include FT_WINFONTS_H
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
                if (li < 2) { // fake bold, as freetype.c's get_bold_glyph_outline (FT_Outline_Embolden is EmboldenXY(s, s)); mono: x only
                    FT_Pos xs = FT_MulDiv(ppem, 1 << 6, 24), ys = li == 0 ? 0 : xs;
                    FT_Error eerr = FT_Outline_EmboldenXY(&wo, xs, ys);
                    if (!call("FT_Outline_EmboldenXY", { slot + (U32)offsetof(FT_GlyphSlotRec, outline), (U32)xs, (U32)ys }) || EAX != (U32)eerr)
                        testFail("FT_Outline_EmboldenXY result");
                    expect(!memory->memcmp(gaddr(gs.outline.points), wo.points, wo.n_points * sizeof(FT_Vector)), "emboldened outline", g, ppem, flags);
                }
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

    // FT_Outline_EmboldenXY trusts the contour ends: an outline FT_Outline_Check rejects is the guest's, an empty one the host's.
    const U32 badPoints = heap + 0x3800, badContours = heap + 0x3900;
    FT_Outline bad = {};
    bad.n_points = 4;
    bad.n_contours = 1;
    bad.points = (FT_Vector*)(uintptr_t)badPoints;
    bad.contours = (short*)(uintptr_t)badContours;
    memory->writew(badContours, 4000); // past the last point
    memory->memcpy(outlineRec, &bad, sizeof(bad));
    if (call("FT_Outline_EmboldenXY", { outlineRec, 64, 64 })) testFail("FT_Outline_EmboldenXY on a contour past the points taken");
    FT_Outline empty = {};
    memory->memcpy(outlineRec, &empty, sizeof(empty));
    if (!call("FT_Outline_EmboldenXY", { outlineRec, 64, 64 }) || EAX != (U32)FT_Outline_EmboldenXY(&empty, 64, 64)) testFail("FT_Outline_EmboldenXY on an empty outline");

    // A debug trap (hardware breakpoint set, or TF) doesn't change who owns a face. The mirror has no driver, so guest
    // FT_Load_Glyph would fault on driver->root.library and guest FT_Done_Face would fail and leak the host face.
    if (!call("FT_New_Memory_Face", { GUEST_LIB, font, (U32)data.size(), 0, aface }) || EAX) testFail("second FT_New_Memory_Face not done on the host");
    U32 face2 = memory->readd(aface);
    if (guest<FT_FaceRec>(face).driver) testFail("the mirror has a driver: guest code could run on it");
    cpu->debugTrapActive = true;
    U32 gi = FT_Get_Char_Index(want, 'g');
    FT_Error terr = FT_Load_Glyph(want, gi, FT_LOAD_NO_HINTING);
    if (!call("FT_Load_Glyph", { face, gi, FT_LOAD_NO_HINTING }) || EAX != (U32)terr) testFail("FT_Load_Glyph on a host face left to the guest under a debug trap");
    U32 tslot = gaddr(guest<FT_FaceRec>(face).glyph);
    if (guest<FT_GlyphSlotRec>(tslot).outline.n_points != want->glyph->outline.n_points) testFail("slot not synced under a debug trap");
    terr = FT_Render_Glyph(want->glyph, FT_RENDER_MODE_NORMAL);
    if (!call("FT_Render_Glyph", { tslot, FT_RENDER_MODE_NORMAL }) || EAX != (U32)terr) testFail("FT_Render_Glyph on a host slot left to the guest under a debug trap");
    if (!call("FT_Done_Face", { face2 }) || EAX) testFail("FT_Done_Face on a host face left to the guest under a debug trap");
    // what is still open goes to the guest, whose functions are valid for it and step through its watched writes
    if (call("FT_New_Memory_Face", { GUEST_LIB, font, (U32)data.size(), 0, aface })) testFail("new face taken under a debug trap");
    if (call("FT_Outline_Get_Bitmap", { GUEST_LIB, tslot + (U32)offsetof(FT_GlyphSlotRec, outline), bitmapRec })) testFail("FT_Outline_Get_Bitmap taken under a debug trap");
    if (call("FT_Outline_EmboldenXY", { tslot + (U32)offsetof(FT_GlyphSlotRec, outline), 64, 64 })) testFail("FT_Outline_EmboldenXY taken under a debug trap");
    cpu->debugTrapActive = false;
    if (call("FT_Load_Glyph", { face2, 1, 0 })) testFail("call on a face freed under a debug trap taken");

    // objects the host didn't create stay guest code
    if (call("FT_Load_Glyph", { heap + 0x3000, 1, 0 }) || call("FT_Render_Glyph", { heap + 0x3000, 0 }) || call("FT_Done_Face", { heap + 0x3000 }))
        testFail("call on a guest face taken");
    if (!call("FT_Done_Face", { face }) || EAX) testFail("FT_Done_Face");
    if (call("FT_Load_Glyph", { face, 1, 0 })) testFail("call on a freed face taken");
    FT_Done_Face(want);
    FT_Done_FreeType(lib);
    hostFreetypeTestCaller = 0;
}

namespace {

// A Windows FNT 2.0 font: three chars, widths 3, 20 and 6, 5 rows, all bits differing. Wine loads .fon files through the
// same driver, and a new binary font isn't worth checking in for a mirror test.
std::vector<U8> fntFont() {
    const U16 widths[3] = { 3, 20, 6 };
    const U32 rows = 5, table = 118, bits = table + 4 * 4;
    std::vector<U8> f(bits);
    auto put16 = [&](U32 at, U32 v) { f[at] = (U8)v; f[at + 1] = (U8)(v >> 8); };
    auto put32 = [&](U32 at, U32 v) { put16(at, v); put16(at + 2, v >> 16); };
    U32 at = bits;
    U8 value = 0x35;
    for (U32 i = 0; i < 3; i++) { // char table: width, offset of its columns
        put16(table + i * 4, widths[i]);
        put16(table + i * 4 + 2, at);
        U32 bytes = (widths[i] + 7) / 8 * rows;
        for (U32 b = 0; b < bytes; b++) f.push_back(value += 0x4b);
        at += bytes;
    }
    put16(table + 3 * 4, 0);
    put16(table + 3 * 4 + 2, at);
    U32 name = (U32)f.size();
    for (const char* p = "Test"; *p; p++) f.push_back((U8)*p);
    f.push_back(0);
    put16(0, 0x200); // version
    put32(2, (U32)f.size()); // file size
    put16(68, 10); // points
    put16(70, 96);
    put16(72, 96); // resolution
    put16(74, 4); // ascent
    put16(83, 400); // weight
    put16(88, rows); // pixel height
    put16(91, 8); // average width
    put16(93, 20); // max width
    f[95] = 'A'; // first char
    f[96] = 'C'; // last char
    put32(105, name); // face name offset
    return f;
}

}

// Mirrors the glyph test doesn't reach, each with its failure in a game: GetFontData's FT_Load_Sfnt_Table shapes (a wrong
// branch or offset: wrong table bytes), FT_Get_Sfnt_Name (a host pointer or a new copy per call), a bitmap font's slot and
// header (.fon text: a stale or too small bitmap), and FT_Load_Glyph's own memory (it used up guest address space, finding 86).
// Not run here: FT_Get_First_Char/Next_Char and FT_Property_Set (one-line forwards), FT_Load_Sfnt_Table's Invalid_Argument
// for a NULL buffer (where the guest would fault; a shortcut, no game does it).
void testHostFreetypeTables() {
    testNewInstruction(0);
    for (int s : { CS, DS, ES, SS }) cpu->seg[s].address = 0;
    std::vector<U8> data;
    FILE* file = fopen("/hostft/Cousine-Regular.ttf", "rb");
    if (!file) testFail("no test font");
    for (int c; (c = fgetc(file)) != EOF;) data.push_back((U8)c);
    fclose(file);
    heap = memory->mmap(cpu->thread, 0, 0x200000, K_PROT_READ | K_PROT_WRITE, K_MAP_PRIVATE | K_MAP_ANONYMOUS, -1, 0);
    stack = heap + 0x1000;
    aface = heap + 0x2000;
    const U32 lenAddr = heap + 0x2100, nameRec = heap + 0x2200, headerRec = heap + 0x2300, out = heap + 0x10000, unmapped = 0x50000000;
    font = heap + 0x100000;
    memory->memcpy(font, data.data(), (U32)data.size());
    FT_Library lib;
    FT_Face want;
    FT_Init_FreeType(&lib);
    if (FT_New_Memory_Face(lib, data.data(), (FT_Long)data.size(), 0, &want)) testFail("host FreeType can't open the test font");
    hostFreetypeTestCaller = RET;
    if (!call("FT_New_Memory_Face", { GUEST_LIB, font, (U32)data.size(), 0, aface }) || EAX) testFail("FT_New_Memory_Face not done on the host");
    U32 face = memory->readd(aface);

    for (FT_ULong tag : { (FT_ULong)0, (FT_ULong)TTAG_head, (FT_ULong)TTAG_cmap }) { // 0: the whole file
        // *length 0 asks for the size
        FT_ULong size = 0;
        if (FT_Load_Sfnt_Table(want, tag, 0, nullptr, &size) || !size || size > 0xF0000) testFail("host FreeType has no table %lx", tag);
        memory->writed(lenAddr, 0);
        if (!call("FT_Load_Sfnt_Table", { face, (U32)tag, 0, 0, lenAddr }) || EAX || memory->readd(lenAddr) != size) testFail("size of table %lx", tag);
        // length NULL reads the whole table
        std::vector<U8> bytes(size);
        FT_Load_Sfnt_Table(want, tag, 0, bytes.data(), nullptr);
        memory->memset(out, 0xCC, (U32)size + 16);
        if (!call("FT_Load_Sfnt_Table", { face, (U32)tag, 0, out, 0 }) || EAX || memory->memcmp(out, bytes.data(), (U32)size)) testFail("whole table %lx", tag);
        if (memory->readb(out + (U32)size) != 0xCC) testFail("wrote past table %lx", tag);
        // length and offset read that part only, and leave *length alone
        U32 offset = (U32)size / 3, n = std::min((U32)size - offset, 40u);
        memory->memset(out, 0xCC, n + 16);
        memory->writed(lenAddr, n);
        if (!call("FT_Load_Sfnt_Table", { face, (U32)tag, offset, out, lenAddr }) || EAX || memory->memcmp(out, bytes.data() + offset, n)) testFail("part of table %lx", tag);
        if (memory->readb(out + n) != 0xCC || memory->readd(lenAddr) != n) testFail("past the part of table %lx", tag);
    }
    // Errors as FreeType's, with nothing written: a table the font doesn't have, and a read past the end of the file.
    memory->memset(out, 0xCC, 16);
    FT_ULong size = 4;
    FT_Error err = FT_Load_Sfnt_Table(want, FT_MAKE_TAG('n', 'o', 'p', 'e'), 0, nullptr, &size);
    memory->writed(lenAddr, 4);
    if (!err || !call("FT_Load_Sfnt_Table", { face, FT_MAKE_TAG('n', 'o', 'p', 'e'), 0, out, lenAddr }) || EAX != (U32)err) testFail("missing table");
    U8 past[8];
    size = 8;
    err = FT_Load_Sfnt_Table(want, 0, (FT_Long)data.size() - 4, past, &size);
    memory->writed(lenAddr, 8);
    if (!err || !call("FT_Load_Sfnt_Table", { face, 0, (U32)data.size() - 4, out, lenAddr }) || EAX != (U32)err) testFail("read past the file");
    if (memory->readd(out) != 0xCCCCCCCC) testFail("failed read wrote to the buffer");
    // A buffer that isn't mapped is an error, not a copy into nowhere.
    memory->writed(lenAddr, 4);
    if (!call("FT_Load_Sfnt_Table", { face, TTAG_head, 0, unmapped, lenAddr }) || !EAX) testFail("read into an unmapped buffer");

    FT_UInt names = FT_Get_Sfnt_Name_Count(want);
    if (!names || !call("FT_Get_Sfnt_Name_Count", { face }) || EAX != names) testFail("FT_Get_Sfnt_Name_Count");
    for (FT_UInt i = 0; i < names; i++) {
        FT_SfntName w;
        FT_Get_Sfnt_Name(want, i, &w);
        if (!call("FT_Get_Sfnt_Name", { face, i, nameRec }) || EAX) testFail("FT_Get_Sfnt_Name %u", i);
        FT_SfntName g = guest<FT_SfntName>(nameRec);
        if (g.platform_id != w.platform_id || g.encoding_id != w.encoding_id || g.language_id != w.language_id || g.name_id != w.name_id ||
            g.string_len != w.string_len || (w.string_len && memory->memcmp(gaddr(g.string), w.string, w.string_len))) testFail("sfnt name %u differs", i);
        // asking again for the same name takes no more guest memory
        if (!call("FT_Get_Sfnt_Name", { face, i, nameRec }) || gaddr(guest<FT_SfntName>(nameRec).string) != gaddr(g.string)) testFail("sfnt name %u copied again", i);
    }
    memory->memset(nameRec, 0xCC, sizeof(FT_SfntName));
    FT_SfntName missing;
    err = FT_Get_Sfnt_Name(want, names, &missing);
    if (!err || !call("FT_Get_Sfnt_Name", { face, names, nameRec }) || EAX != (U32)err || memory->readd(nameRec) != 0xCCCCCCCC) testFail("name past the last");
    // a TrueType font has no FNT header
    FT_WinFNT_HeaderRec wh, gh;
    err = FT_Get_WinFNT_Header(want, &wh);
    if (!err || !call("FT_Get_WinFNT_Header", { face, headerRec }) || EAX != (U32)err) testFail("FNT header of a TrueType font");

    // FT_Load_Glyph with no bitmap in the slot, as all of win32u's text: it may not take guest memory each time (16 bytes
    // per call when a buffer stayed empty, a new 64 KB mapping per 4096 calls, and text gone when address space ran out).
    U32 gi = FT_Get_Char_Index(want, 'g');
    FT_Set_Pixel_Sizes(want, 0, 16); // a load with no size set fails, with an empty slot: not the case this is about
    if (!call("FT_Set_Pixel_Sizes", { face, 0, 16 }) || EAX) testFail("FT_Set_Pixel_Sizes");
    if (FT_Load_Glyph(want, gi, FT_LOAD_NO_HINTING) || !want->glyph->outline.n_points || want->glyph->bitmap.buffer) testFail("glyph is not an outline with no bitmap");
    if (!call("FT_Load_Glyph", { face, gi, FT_LOAD_NO_HINTING }) || EAX) testFail("FT_Load_Glyph: %x", EAX);
    U32 before = memory->mmap(cpu->thread, 0, K_PAGE_SIZE, K_PROT_READ | K_PROT_WRITE, K_MAP_PRIVATE | K_MAP_ANONYMOUS, -1, 0);
    memory->unmap(before, K_PAGE_SIZE);
    for (int i = 0; i < 5000; i++) {
        if (!call("FT_Load_Glyph", { face, gi, FT_LOAD_NO_HINTING }) || EAX) testFail("FT_Load_Glyph %d: %x", i, EAX);
    }
    U32 after = memory->mmap(cpu->thread, 0, K_PAGE_SIZE, K_PROT_READ | K_PROT_WRITE, K_MAP_PRIVATE | K_MAP_ANONYMOUS, -1, 0);
    memory->unmap(after, K_PAGE_SIZE);
    if (after != before) testFail("FT_Load_Glyph took guest memory: next free page %x, was %x", after, before);
    if (!call("FT_Done_Face", { face }) || EAX) testFail("FT_Done_Face");
    FT_Done_Face(want);

    // A bitmap font: the slot's bitmap in guest memory, through glyphs small, big and small again (its buffer grows once).
    std::vector<U8> fnt = fntFont();
    if (FT_New_Memory_Face(lib, fnt.data(), (FT_Long)fnt.size(), 0, &want)) testFail("host FreeType can't open the FNT font");
    U32 fontAddress = heap + 0x1F0000;
    memory->memcpy(fontAddress, fnt.data(), (U32)fnt.size());
    if (!call("FT_New_Memory_Face", { GUEST_LIB, fontAddress, (U32)fnt.size(), 0, aface }) || EAX) testFail("FT_New_Memory_Face of a FNT font");
    face = memory->readd(aface);
    FT_FaceRec fr = guest<FT_FaceRec>(face);
    if (fr.num_fixed_sizes != want->num_fixed_sizes || !fr.num_fixed_sizes || fr.num_glyphs != want->num_glyphs ||
        memory->memcmp(gaddr(fr.available_sizes), want->available_sizes, fr.num_fixed_sizes * sizeof(FT_Bitmap_Size))) testFail("FNT face fields differ");
    if (FT_Get_WinFNT_Header(want, &wh) || !call("FT_Get_WinFNT_Header", { face, headerRec }) || EAX) testFail("FT_Get_WinFNT_Header");
    gh = guest<FT_WinFNT_HeaderRec>(headerRec);
    if (memcmp(&gh, &wh, sizeof(gh))) testFail("FNT header differs");
    err = FT_Set_Pixel_Sizes(want, 0, 5);
    if (!call("FT_Set_Pixel_Sizes", { face, 0, 5 }) || EAX != (U32)err) testFail("FT_Set_Pixel_Sizes on a FNT font");
    for (U32 g : { 1u, 2u, 3u, 1u, 0u }) {
        err = FT_Load_Glyph(want, g, FT_LOAD_DEFAULT);
        if (!call("FT_Load_Glyph", { face, g, FT_LOAD_DEFAULT }) || EAX != (U32)err || err) testFail("FT_Load_Glyph %u of a FNT font", g);
        FT_GlyphSlot ws = want->glyph;
        FT_GlyphSlotRec gs = guest<FT_GlyphSlotRec>(gaddr(guest<FT_FaceRec>(face).glyph));
        U32 len = ws->bitmap.rows * (U32)std::abs(ws->bitmap.pitch);
        if (gs.format != FT_GLYPH_FORMAT_BITMAP || gs.bitmap.rows != ws->bitmap.rows || gs.bitmap.width != ws->bitmap.width || gs.bitmap.pitch != ws->bitmap.pitch ||
            gs.bitmap.pixel_mode != ws->bitmap.pixel_mode || gs.bitmap_top != ws->bitmap_top || memcmp(&gs.metrics, &ws->metrics, sizeof(gs.metrics))) testFail("FNT glyph %u fields differ", g);
        if (!len || memory->memcmp(gaddr(gs.bitmap.buffer), ws->bitmap.buffer, len)) testFail("FNT glyph %u bitmap differs", g);
    }
    if (!call("FT_Done_Face", { face }) || EAX) testFail("FT_Done_Face of a FNT font");
    FT_Done_Face(want);
    FT_Done_FreeType(lib);
    hostFreetypeTestCaller = 0;
    memory->unmap(heap, 0x200000);
}

#endif
