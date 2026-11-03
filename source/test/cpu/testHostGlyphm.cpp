/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "boxedwine.h"

#if defined(__TEST) && !defined(BOXEDWINE_MULTI_THREADED)

// Host glyph metrics (hostglyphm.cpp): text measuring answered from Wine's caches by walking its structures, here built
// by hand with offsets the tables below choose. It has to answer exactly what get_glyph_outline's GGO_METRICS cache hit
// gives (Wine 11.0 font.c), and any other call must write nothing and stay with the guest: a wrong rule gives wrong
// layout with no error, misplaced text in some game. Real fonts, off against on, were compared in a browser when this
// code was written; this pins the rules, and needs no font, so a later change can't move them silently.
#include "testHostGlyphm.h"
#include "testCPU.h"

bool hostGlyphmRun(CPU* cpu, U32 fn);
void hostGlyphmTestTables(const U32* info, const U32* ft);

#define cpu (testContext().cpu)
#define memory (testContext().memory)

namespace {

enum : U32 { EXTENT, EXTENT_I, GLYPH_OUTLINE };
constexpr U32 V_EXTENT = 0x100, V_EXTENT_I = 0x200, V_OUTLINE = 0x300, V_LOCK = 0x400, V_FONT_FUNCS = 0x404, V_BACKEND = 0x500;
constexpr U32 PHYSDEV_FONT = 8, FONT_ENTRY = 0x10, FONT_GM_SIZE = 0x20, FONT_GM = 0x24, FONT_PRIVATE = 0x28, FONT_CHILDREN = 0x30, FONT_LF_FACE = 0x40, FONT_FAMILY = 0x80;
constexpr U32 GM_BLOCK = 16, GM_SIZE = 40, GM_GM = 0, GM_ABC = 20, GM_INIT = 32;
constexpr U32 DATA_FACE = 4, DATA_CHAR_INDEX = 8, FACE_CHARMAP = 0x10, CHARMAP_ENCODING = 8, ENC_NONE = 0, ENC_UNICODE = 0x756e6963, ENC_SYMBOL = 0x73796d62;
// vnbox_glyphm and vnbox_glyphm_ft, in the patch's order (vn/wine/win32u-glyphmetrics-export.patch)
const U32 infoTable[] = { 1, V_EXTENT, V_EXTENT_I, V_OUTLINE, V_LOCK, V_FONT_FUNCS, PHYSDEV_FONT, FONT_ENTRY, FONT_GM_SIZE, FONT_GM, FONT_PRIVATE,
    FONT_CHILDREN, FONT_LF_FACE, FONT_FAMILY, GM_BLOCK, GM_SIZE, GM_GM, GM_ABC, GM_INIT };
const U32 ftTable[] = { 1, V_BACKEND, DATA_FACE, DATA_CHAR_INDEX, FACE_CHARMAP, CHARMAP_ENCODING, ENC_NONE, ENC_SYMBOL };

constexpr U32 RET = 0x12345678, SENTINEL = 0xAAAAAAAA;
U32 heap, next, base, stack, dev, dxs, str;

U32 alloc(U32 len) {
    U32 a = next;
    next += (len + 15) & ~15u;
    return a;
}

// fn called as the guest's cdecl call arrives; true when answered on the host.
bool run(U32 fn, std::initializer_list<U32> args) {
    const U32 vaddr[3] = { V_EXTENT, V_EXTENT_I, V_OUTLINE };
    ESP = stack;
    memory->writed(stack, RET);
    U32 a = stack + 4;
    for (U32 v : args) { memory->writed(a, v); a += 4; }
    EAX = 0xDEADBEEF;
    const U32 eip = base + vaddr[fn];
    cpu->eip.u32 = eip;
    bool ran = hostGlyphmRun(cpu, fn);
    if (ran && (ESP != stack + 4 || cpu->eip.u32 != RET || EAX != 1)) testFail("fn %u didn't return TRUE like `ret`", fn);
    if (!ran && (ESP != stack || cpu->eip.u32 != eip || EAX != 0xDEADBEEF)) testFail("declined fn %u changed registers", fn);
    return ran;
}

U32 newFont(U32 encoding = ENC_UNICODE) {
    U32 font = alloc(0x100), data = alloc(DATA_CHAR_INDEX + 256 * 4), face = alloc(0x20), charmap = alloc(16), family = alloc(8);
    memory->writed(font + FONT_PRIVATE, data);
    memory->writed(font + FONT_CHILDREN, font + FONT_CHILDREN); // empty list
    memory->writed(font + FONT_FAMILY, family);
    memory->writew(family, 'A');
    memory->writew(font + FONT_LF_FACE, 'A');
    memory->writed(data + DATA_FACE, face);
    memory->writed(face + FACE_CHARMAP, charmap);
    memory->writed(charmap + CHARMAP_ENCODING, encoding);
    U32 blocks = alloc(4 * 4);
    memory->writed(font + FONT_GM_SIZE, 4);
    memory->writed(font + FONT_GM, blocks);
    return font;
}

// char_index: ch -> glyph, as win32u-glyph-cache.patch stores it (glyph + 1, 0 = not cached).
void setChar(U32 font, U32 ch, U32 glyph) {
    U32 slot = memory->readd(font + FONT_PRIVATE) + DATA_CHAR_INDEX + (ch >> 8) * 4;
    U32 block = memory->readd(slot);
    if (!block) {
        block = alloc(256 * 4);
        memory->writed(slot, block);
    }
    memory->writed(block + (ch & 0xff) * 4, glyph + 1);
}

// font->gm[glyph / GM_BLOCK][glyph % GM_BLOCK] with init set; the 20 gm bytes are all `fill`.
void setMetrics(U32 font, U32 glyph, S32 a, U32 b, S32 c, U8 fill = 0x77) {
    U32 slot = memory->readd(font + FONT_GM) + glyph / GM_BLOCK * 4;
    U32 entries = memory->readd(slot);
    if (!entries) {
        entries = alloc(GM_BLOCK * GM_SIZE);
        memory->writed(slot, entries);
    }
    U32 e = entries + glyph % GM_BLOCK * GM_SIZE;
    memory->memset(e + GM_GM, fill, 20);
    memory->writed(e + GM_ABC, (U32)a);
    memory->writed(e + GM_ABC + 4, b);
    memory->writed(e + GM_ABC + 8, (U32)c);
    memory->writed(e + GM_INIT, 1);
}

void addChild(U32 font, U32 child) {
    U32 head = font + FONT_CHILDREN, entry = child + FONT_ENTRY, last = head;
    while (memory->readd(last) != head) last = memory->readd(last);
    memory->writed(last, entry);
    memory->writed(entry, head);
}

void useFont(U32 font) { memory->writed(dev + PHYSDEV_FONT, font); }

void putString(const std::vector<U16>& s) {
    for (size_t i = 0; i < s.size(); i++) memory->writew(str + (U32)i * 2, s[i]);
}

void clearDxs() { for (U32 i = 0; i < 8; i++) memory->writed(dxs + i * 4, SENTINEL); }

// A call that must be answered with these running totals in dxs and nothing past them.
void expectDxs(U32 fn, const std::vector<U16>& s, const std::vector<S32>& want, const char* what) {
    putString(s);
    clearDxs();
    if (!run(fn, { dev, str, (U32)s.size(), dxs })) testFail("%s: left to the guest", what);
    for (size_t i = 0; i < want.size(); i++) {
        if ((S32)memory->readd(dxs + (U32)i * 4) != want[i]) testFail("%s: dx %d is %d, not %d", what, (int)i, (S32)memory->readd(dxs + (U32)i * 4), want[i]);
    }
    if (memory->readd(dxs + (U32)want.size() * 4) != SENTINEL) testFail("%s: wrote past the last dx", what);
}

// A call that must stay with the guest, with no dx written (a half-written answer followed by the guest's is the bug).
void expectGuest(U32 fn, const std::vector<U16>& s, const char* what) {
    putString(s);
    clearDxs();
    if (run(fn, { dev, str, (U32)s.size(), dxs })) testFail("%s: answered on the host", what);
    for (U32 i = 0; i < 8; i++) {
        if (memory->readd(dxs + i * 4) != SENTINEL) testFail("%s: wrote dx %u", what, i);
    }
}

}

void testHostGlyphmAnswers() {
    testNewInstruction(0);
    for (int s : { CS, DS, ES, SS }) cpu->seg[s].address = 0;
    heap = memory->mmap(cpu->thread, 0, 0x100000, K_PROT_READ | K_PROT_WRITE, K_MAP_PRIVATE | K_MAP_ANONYMOUS, -1, 0);
    static std::vector<U8> zero(0x100000);
    memory->memcpy(heap, zero.data(), 0x100000); // RAM on every page: the host reads only what is already RAM
    base = heap + 0x1000;
    stack = heap + 0x800;
    next = heap + 0x2000;
    memory->writed(base + V_FONT_FUNCS, base + V_BACKEND);
    dev = alloc(0x20);
    dxs = alloc(32);
    str = alloc(64);
    hostGlyphmTestTables(infoTable, ftTable);

    U32 font = newFont();
    useFont(font);
    setChar(font, 'A', 3);
    setChar(font, 'B', 17); // the second gm block: a block or entry index mixed up shows here
    setMetrics(font, 3, 1, 10, 2);
    setMetrics(font, 17, -2, 20, 1); // a negative A
    // A full cache gives the running total of A + B + C, per char.
    expectDxs(EXTENT, { 'A', 'B', 'A' }, { 13, 32, 45 }, "full cache");
    // ExPointI takes glyph indices: the chars' table isn't consulted, and neither are linked fonts.
    expectDxs(EXTENT_I, { 17, 3 }, { 19, 32 }, "ExPointI");

    // A metrics hole in the middle: the guest fills the cache, and no half answer is left behind.
    setChar(font, 'C', 5);
    expectGuest(EXTENT, { 'A', 'C', 'B' }, "metrics hole");
    // (init clear in a block that exists: the same hole, read from the entry)
    setChar(font, 'D', 18);
    expectGuest(EXTENT, { 'A', 'D' }, "init clear");
    expectGuest(EXTENT_I, { 3, 18 }, "ExPointI init clear");
    // A glyph past font->gm's size isn't read out of bounds.
    setChar(font, 'E', GM_BLOCK * 4);
    expectGuest(EXTENT, { 'E' }, "glyph past gm_size");
    // A char not in char_index, in a block that exists and in one that doesn't.
    expectGuest(EXTENT, { 'Z' }, "char not cached");
    expectGuest(EXTENT, { 0x4e00 }, "char block not cached");
    expectGuest(EXTENT, {}, "count 0");

    // get_glyph_index_linked: a printable char the font lacks is looked up in the child fonts, and the child's metrics
    // answer; a child that has it wins over the ones after it.
    U32 child1 = newFont(), child2 = newFont();
    addChild(font, child1);
    addChild(font, child2);
    setChar(font, 'L', 0);
    setChar(child1, 'L', 0);
    setChar(child2, 'L', 7);
    setMetrics(child2, 7, 0, 30, 0);
    setMetrics(font, 0, 0, 5, 0); // the parent's glyph 0
    expectDxs(EXTENT, { 'L', 'A' }, { 30, 43 }, "char from a child font");
    // No child has it: the parent's glyph 0 (the missing-char box).
    setChar(font, 'N', 0);
    setChar(child1, 'N', 0);
    setChar(child2, 'N', 0);
    expectDxs(EXTENT, { 'N' }, { 5 }, "char in no font");
    // A control char isn't looked up in the children, even when one has it.
    setChar(font, 5, 0);
    setChar(child2, 5, 7);
    expectDxs(EXTENT, { 5 }, { 5 }, "control char");
    setChar(font, 32, 0);
    setChar(child1, 32, 0);
    setChar(child2, 32, 7);
    expectDxs(EXTENT, { 32 }, { 30 }, "space is not a control char");
    // A child not loaded yet, or that hasn't cached this char: the guest loads it.
    setChar(font, 'M', 0);
    expectGuest(EXTENT, { 'M' }, "char not cached in a child");
    U32 unloaded = newFont();
    memory->writed(unloaded + FONT_PRIVATE, 0);
    memory->writed(child1 + FONT_ENTRY, unloaded + FONT_ENTRY);
    memory->writed(unloaded + FONT_ENTRY, child2 + FONT_ENTRY);
    expectGuest(EXTENT, { 'N' }, "child font not loaded");
    // A corrupt list can't hang the emulator or walk off the end.
    memory->writed(child1 + FONT_ENTRY, child1 + FONT_ENTRY);
    memory->writed(font + FONT_CHILDREN, child1 + FONT_ENTRY);
    setChar(child1, 'N', 0);
    expectGuest(EXTENT, { 'N' }, "child list loop");
    memory->writed(child1 + FONT_ENTRY, 0);
    expectGuest(EXTENT, { 'N' }, "null child entry");
    memory->writed(font + FONT_CHILDREN, font + FONT_CHILDREN);

    // Guest code may be changing the structures, or the backend isn't FreeType's; or this physdev has no font.
    memory->writed(base + V_LOCK, 1);
    expectGuest(EXTENT, { 'A' }, "font_lock held");
    memory->writed(base + V_LOCK, 0);
    memory->writed(base + V_FONT_FUNCS, base + V_BACKEND + 4);
    expectGuest(EXTENT, { 'A' }, "other backend");
    memory->writed(base + V_FONT_FUNCS, base + V_BACKEND);
    useFont(0);
    expectGuest(EXTENT, { 'A' }, "no font");
    useFont(font);

    // A vertical ('@') font: chars go through GSUB's vert table, which isn't here; glyph indices don't.
    memory->writew(font + FONT_LF_FACE, '@');
    expectGuest(EXTENT, { 'A' }, "'@' face name");
    expectDxs(EXTENT_I, { 3 }, { 13 }, "ExPointI on a '@' font");
    memory->writew(font + FONT_LF_FACE, 'A');
    memory->writew(memory->readd(font + FONT_FAMILY), '@');
    expectGuest(EXTENT, { 'A' }, "'@' family name");
    memory->writew(memory->readd(font + FONT_FAMILY), 'A');

    // Symbol fonts map chars differently (no char_index), but their glyph indices are fine; a no-encoding font's aren't.
    U32 symbol = newFont(ENC_SYMBOL);
    setChar(symbol, 'A', 3);
    setMetrics(symbol, 3, 1, 10, 2);
    useFont(symbol);
    expectGuest(EXTENT, { 'A' }, "symbol font");
    expectDxs(EXTENT_I, { 3 }, { 13 }, "ExPointI on a symbol font");
    U32 noEncoding = newFont(ENC_NONE);
    setChar(noEncoding, 'A', 3);
    setMetrics(noEncoding, 3, 1, 10, 2);
    useFont(noEncoding);
    expectGuest(EXTENT, { 'A' }, "no-encoding font");
    expectGuest(EXTENT_I, { 3 }, "ExPointI on a no-encoding font");
    useFont(font);

    // Bad arguments: nothing mapped there, or a count beyond reason.
    putString({ 'A' });
    if (run(EXTENT, { dev, 0x50000000, 1, dxs }) || run(EXTENT, { dev, str, 1, 0x50000000 }) || run(EXTENT, { dev, str, (U32)-1, dxs })) testFail("bad arguments answered");
    // A string across a page end is read whole.
    U32 pageEnd = (heap + 0x80000) - 2;
    memory->writew(pageEnd, 'A');
    memory->writew(pageEnd + 2, 'B');
    clearDxs();
    if (!run(EXTENT, { dev, pageEnd, 2, dxs }) || memory->readd(dxs + 4) != 32) testFail("string across a page end");
    // A debug trap: the host's writes wouldn't fire a watchpoint, and the guest does the same job.
    cpu->debugTrapActive = true;
    expectGuest(EXTENT, { 'A' }, "debug trap");
    cpu->debugTrapActive = false;

    // font_GetGlyphOutline(dev, glyph, format, gm, buflen, buf, mat): GGO_METRICS answered from the same cache.
    constexpr U32 GGO_METRICS = 0, GGO_BITMAP = 1, GGO_GLYPH_INDEX = 0x80;
    U32 gmOut = alloc(32), mat = alloc(16);
    setMetrics(font, 3, 1, 10, 2, 0x31);
    setMetrics(font, 17, -2, 20, 1, 0x41);
    auto gmIs = [&](U8 fill) {
        for (U32 i = 0; i < 20; i++) if (memory->readb(gmOut + i) != fill) return false;
        return memory->readb(gmOut + 20) == 0xEE; // and nothing past GLYPHMETRICS
    };
    auto outline = [&](U32 glyph, U32 format, U32 matrix) {
        memory->memset(gmOut, 0xEE, 32);
        return run(GLYPH_OUTLINE, { dev, glyph, format, gmOut, 0, 0, matrix });
    };
    if (!outline('A', GGO_METRICS, 0) || !gmIs(0x31)) testFail("GGO_METRICS by char");
    if (!outline(17, GGO_METRICS | GGO_GLYPH_INDEX, 0) || !gmIs(0x41)) testFail("GGO_METRICS by glyph index");
    // Through the linked fonts, as a string does.
    setMetrics(child2, 7, 0, 30, 0, 0x51);
    addChild(font, child2);
    if (!outline('L', GGO_METRICS, 0) || !gmIs(0x51)) testFail("GGO_METRICS from a child font");
    // No gm wanted: answered, nothing written.
    memory->memset(gmOut, 0xEE, 32);
    if (!run(GLYPH_OUTLINE, { dev, 'A', GGO_METRICS, 0, 0, 0, 0 }) || memory->readb(gmOut) != 0xEE) testFail("GGO_METRICS without a gm");
    // The identity matrix is no matrix; any other isn't in the cache.
    const U8 identity[16] = { 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0 };
    memory->memcpy(mat, identity, 16);
    if (!outline('A', GGO_METRICS, mat) || !gmIs(0x31)) testFail("GGO_METRICS with the identity matrix");
    memory->writeb(mat + 2, 2);
    if (outline('A', GGO_METRICS, mat) || memory->readb(gmOut) != 0xEE) testFail("a matrix answered");
    // Anything but metrics, and chars char_index can't hold.
    if (outline('A', GGO_BITMAP, 0)) testFail("GGO_BITMAP answered");
    if (outline(0x10000, GGO_METRICS, 0)) testFail("a char over 0xffff answered");
    // A vertical font's chars; its glyph indices are fine.
    memory->writew(font + FONT_LF_FACE, '@');
    if (outline('A', GGO_METRICS, 0)) testFail("GGO_METRICS on a '@' font");
    if (!outline(3, GGO_METRICS | GGO_GLYPH_INDEX, 0) || !gmIs(0x31)) testFail("GGO_GLYPH_INDEX on a '@' font");
    memory->writew(font + FONT_LF_FACE, 'A');
    // A symbol font takes glyph indices, a no-encoding font goes to the guest.
    useFont(symbol);
    if (outline('A', GGO_METRICS, 0)) testFail("GGO_METRICS by char on a symbol font");
    if (!outline(3, GGO_METRICS | GGO_GLYPH_INDEX, 0)) testFail("GGO_GLYPH_INDEX on a symbol font");
    useFont(noEncoding);
    if (outline(3, GGO_METRICS | GGO_GLYPH_INDEX, 0)) testFail("GGO_GLYPH_INDEX on a no-encoding font");
    useFont(font);
    // An unwritable gm: nothing is half answered.
    if (run(GLYPH_OUTLINE, { dev, 'A', GGO_METRICS, 0x50000000, 0, 0, 0 })) testFail("unwritable gm answered");

    hostGlyphmTestTables(nullptr, nullptr);
    memory->unmap(heap, 0x100000);
}

#endif
