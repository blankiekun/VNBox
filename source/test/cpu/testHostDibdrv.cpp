/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "boxedwine.h"

#if defined(__TEST) && defined(BOXEDWINE_HOST_DIBDRV)

// Host DIB primitives (hostdibdrv.cpp): a call runs on a host copy of just the rows and bytes it touches, with dib_info's
// rect, stride and bits moved to match. The reference is the same primitive run directly on a host copy of the whole
// bitmap, so a slip in that math shows as different bytes in guest memory, on some bit depth only, and in games only as a
// glitch far from the cause. Catches: bottom-up rows mapped wrongly, 24 bpp's 12-byte alignment or the bit position of
// 1/4 bpp lost, merged regions of an in-place blit, bytes next to the rect overwritten, a source written back, a span of
// stretch_row/shrink_row too short, and a color table or funcs table not moved to the host.
// Not run here: file pages not loaded yet (guest), gradient_rect and draw_subpixel_glyph (the same single-region path as
// solid_rects and draw_glyph), and whether the primitives themselves are right (Wine's code, compared by cmp with its tarball).
#include "testHostDibdrv.h"
#include "testCPU.h"

bool hostDibdrvRun(CPU* cpu, U32 index);
U32 hostDibdrvTestEntry(U32 table, U32 slot, U32 tableVaddr, U32 vaddr);
extern "C" void (*vnbox_dibdrv_host_fn(unsigned table, unsigned slot))(void);
extern "C" const void* vnbox_dibdrv_host_table(unsigned table);

#define cpu (testContext().cpu)
#define memory (testContext().memory)

namespace {

// primitive_funcs' order (lib/wine-dibdrv/dibdrv.h)
enum : U32 {
    SOLID_RECTS = 0, COPY_RECT = 3, BLEND_RECTS = 4, MASK_RECT = 6, DRAW_GLYPH = 7, GET_PIXEL = 9, COLORREF_TO_PIXEL = 10,
    PIXEL_TO_COLORREF = 11, CONVERT_TO = 12, STRETCH_ROW = 15, SHRINK_ROW = 16, SLOTS = 18
};
enum : U32 { R2_XORPEN = 7, R2_COPYPEN = 13, OVERLAP_LEFT = 1, OVERLAP_RIGHT = 2, OVERLAP_ABOVE = 4, OVERLAP_BELOW = 8, STRETCH_ANDSCANS = 1, STRETCH_DELETESCANS = 3 };
const U32 tableBpp[9] = { 32, 32, 24, 16, 16, 8, 4, 1, 0 }; // funcs_8888, _32, _24, _555, _16, _8, _4, _1, _null
constexpr U32 T_8888 = 0, T_24 = 2, T_555 = 3, T_8 = 5, T_4 = 6, T_1 = 7;
constexpr U32 LOAD_BASE = 0x10000000, FN_VADDR = 0x1000, TABLE_VADDR = 0x2000, RET = 0x12345678;
// dib_info's i386 layout (lib/wine-dibdrv/host.c checks it)
constexpr U32 DI_BITCOUNT = 0, DI_WIDTH = 4, DI_HEIGHT = 8, DI_RECT = 16, DI_STRIDE = 32, DI_BITS = 36, DI_SHIFTS = 64, DI_LENS = 76, DI_COLORS = 88, DI_COLORCOUNT = 92, DI_FUNCS = 96, DI_SIZE = 100;

struct Rect { S32 left, top, right, bottom; };
struct Point { S32 x, y; };
struct Blend { U8 op, flags, constantAlpha, alphaFormat; };
struct StretchParams { S32 errStart, errAdd1, errAdd2; U32 length; S32 dstInc, srcInc; };

U32 entries[9][SLOTS];
U32 next, stack;
U32 rngState;

U32 rng() { return (rngState = rngState * 1664525 + 1013904223) >> 8; }

U32 alloc(U32 len) {
    U32 a = next;
    next += (len + 15) & ~15u;
    return a;
}

U32 pitchOf(U32 bpp, S32 w) { return (w * bpp + 31) / 32 * 4; }

// A bitmap in guest memory and its reference on the host, same bytes; both dib_infos as win32u and host.c want them.
struct Bmp {
    U32 table = 0, bpp = 0, guest = 0, infoAddr = 0, pitch = 0;
    S32 w = 0, h = 0, stride = 0;
    std::vector<U8> host;
    U8 hostInfo[DI_SIZE] = {};
    U32 hostColors[256];

    static void put(U8* info, U32 offset, U32 v) { ::memcpy(info + offset, &v, 4); }
    U8* hostBits(S32 rows = 0) { return host.data() + (S64)(stride > 0 ? 0 : (h - 1) * (S64)pitch) + (S64)rows * stride; }
    U32 guestBits(S32 rows = 0) { return (U32)((S64)guest + (stride > 0 ? 0 : (h - 1) * (S64)pitch) + (S64)rows * stride); }

    // Writes both dib_infos; colorTable false / funcs 0: the guest's fields stay garbage, as a glyph's dib_info does.
    void writeInfo(const Rect& rect, bool colorTable = true, bool funcs = true, S32 rows = 0) {
        U8 info[DI_SIZE] = {};
        put(info, DI_BITCOUNT, bpp);
        put(info, DI_WIDTH, w);
        put(info, DI_HEIGHT, h);
        ::memcpy(info + DI_RECT, &rect, 16);
        put(info, DI_STRIDE, (U32)stride);
        if (table == T_555) { // 5-5-5 masks, as win32u fills them for the 16 bpp tables
            const U32 shifts[3] = { 10, 5, 0 };
            for (U32 i = 0; i < 3; i++) {
                put(info, DI_SHIFTS + i * 4, shifts[i]);
                put(info, DI_LENS + i * 4, 5);
            }
        }
        U32 colorCount = colorTable && bpp <= 8 ? 1u << bpp : 0;
        put(info, DI_COLORCOUNT, colorCount);
        ::memcpy(hostInfo, info, DI_SIZE);
        if (!colorTable) put(info, DI_COLORCOUNT, 0xFFFF);
        put(hostInfo, DI_BITS, (U32)(uintptr_t)hostBits(rows));
        put(hostInfo, DI_COLORS, colorCount ? (U32)(uintptr_t)hostColors : 0);
        put(hostInfo, DI_FUNCS, funcs ? (U32)(uintptr_t)vnbox_dibdrv_host_table(table) : 0);
        put(info, DI_BITS, guestBits(rows));
        U32 guestColors = 0;
        if (colorCount) {
            guestColors = alloc(colorCount * 4);
            memory->memcpy(guestColors, hostColors, colorCount * 4);
        }
        put(info, DI_COLORS, colorTable ? guestColors : 0xDEAD0000);
        put(info, DI_FUNCS, funcs ? LOAD_BASE + TABLE_VADDR + table * 0x100 : 0xBAD00000);
        memory->memcpy(infoAddr, info, DI_SIZE);
    }
};

// table's format at a new guest address, random bytes; rect is the dib's visible rect (non-zero for a sub-DIB).
void makeBmp(Bmp& b, U32 table, S32 w, S32 h, int sign, const Rect& rect, U32 bppOverride = 0, bool colorTable = true, bool funcs = true) {
    b.table = table;
    b.bpp = bppOverride ? bppOverride : tableBpp[table];
    b.w = w;
    b.h = h;
    b.pitch = pitchOf(b.bpp, w);
    b.stride = sign * (S32)b.pitch;
    b.host.resize(b.pitch * h);
    for (U8& v : b.host) v = (U8)rng();
    for (U32 i = 0; i < 256; i++) b.hostColors[i] = rng() & 0xffffff;
    b.guest = alloc((U32)b.host.size());
    b.infoAddr = alloc(DI_SIZE);
    memory->memcpy(b.guest, b.host.data(), (U32)b.host.size());
    b.writeInfo(rect, colorTable, funcs);
}

// What the primitive left in guest memory must be what it left in the whole host copy (pixelsOnly: not the row padding,
// a known exception of convert_to's fast path).
void same(const Bmp& b, const char* what, bool pixelsOnly = false) {
    U32 rowBytes = pixelsOnly ? (b.w * b.bpp) / 8 : b.pitch;
    for (S32 y = 0; y < b.h; y++) {
        if (memory->memcmp(b.guest + y * b.pitch, b.host.data() + y * b.pitch, rowBytes)) {
            testFail("%s: %d bpp table %u, stride %d: memory row %d differs", what, b.bpp, b.table, b.stride, y);
        }
    }
}

// The primitive `slot` of table called as the guest's cdecl call arrives; true when done on the host.
bool run(U32 table, U32 slot, std::initializer_list<U32> args) {
    ESP = stack;
    memory->writed(stack, RET);
    U32 a = stack + 4;
    for (U32 v : args) { memory->writed(a, v); a += 4; }
    for (U32 i = (U32)args.size(); i < 7; i++) { memory->writed(a, 0); a += 4; }
    EAX = 0xDEADBEEF;
    cpu->eip.u32 = LOAD_BASE + FN_VADDR;
    bool ran = hostDibdrvRun(cpu, entries[table][slot]);
    if (ran && (ESP != stack + 4 || cpu->eip.u32 != RET)) testFail("slot %u did not return like `ret`", slot);
    if (!ran && (ESP != stack || cpu->eip.u32 != LOAD_BASE + FN_VADDR || EAX != 0xDEADBEEF)) testFail("declined slot %u changed registers", slot);
    return ran;
}

void mustRun(U32 table, U32 slot, std::initializer_list<U32> args) {
    if (!run(table, slot, args)) testFail("slot %u table %u left to the guest", slot, table);
}

U32 guestStruct(const void* p, U32 len) {
    U32 a = alloc(len);
    memory->memcpy(a, p, len);
    return a;
}

void setup() {
    testNewInstruction(0);
    for (int s : { CS, DS, ES, SS }) cpu->seg[s].address = 0;
    static bool entered;
    for (U32 t = 0; t < 8 && !entered; t++) {
        for (U32 s : { SOLID_RECTS, COPY_RECT, BLEND_RECTS, MASK_RECT, DRAW_GLYPH, GET_PIXEL, COLORREF_TO_PIXEL, PIXEL_TO_COLORREF, CONVERT_TO, STRETCH_ROW, SHRINK_ROW }) {
            entries[t][s] = hostDibdrvTestEntry(t, s, TABLE_VADDR + t * 0x100, FN_VADDR);
        }
    }
    entered = true;
    U32 heap = memory->mmap(cpu->thread, 0, 0x100000, K_PROT_READ | K_PROT_WRITE, K_MAP_PRIVATE | K_MAP_ANONYMOUS, -1, 0);
    static std::vector<U8> zero(0x100000);
    memory->memcpy(heap, zero.data(), 0x100000); // RAM on every page, as a drawn-to DIB has
    stack = heap + 0x800;
    next = heap + 0x1000;
    rngState = 0x1234;
}

}

void testHostDibdrvCopies() {
    auto fn = [](U32 table, U32 slot) { return vnbox_dibdrv_host_fn(table, slot); };
    setup();
    const S32 W = 48, H = 20;
    const U32 heapStart = next;
    for (U32 table : { T_8888, T_24, T_555, T_8, T_4, T_1 }) {
        for (int sign : { 1, -1 }) {
            for (Point at : { Point{ 0, 0 }, Point{ 5, 3 } }) { // a sub-DIB moves every coordinate
                next = heapStart;
                const Rect rect = { at.x, at.y, at.x + 32, at.y + 14 };
                static Bmp dst, other, oneBit, argb, glyph;
                makeBmp(dst, table, W, H, sign, rect);
                makeBmp(other, table, W, H, -sign, { at.y, at.x, at.y + 32, at.x + 14 }); // separate regions, the other way up
                makeBmp(oneBit, T_1, W, H, -sign, rect);
                makeBmp(argb, T_8888, W, H, -sign, rect);
                makeBmp(glyph, T_8, W, H, 1, { 0, 0, W, H }, 0, false, false);
                const U32 start = next;
                for (S32 left : { 0, 1, 2, 3, 5, 7, 8, 13 }) { // every byte and DWORD and 24 bpp triplet position
                    for (S32 width : { 1, 2, 3, 9, 16 }) {
                        next = start;
                        S32 top = left % 3;
                        const Rect rc = { left, top, left + width, top + 1 + left % 4 };
                        const U32 grc = guestStruct(&rc, sizeof(rc));

                        // solid_rects, two rects: each is copied in, drawn and copied back on its own.
                        const Rect rcs[2] = { rc, { rc.left + 1, rc.top + 2, rc.right + 1, rc.bottom + 2 } };
                        U32 andv = rng(), xorv = rng();
                        mustRun(table, SOLID_RECTS, { dst.infoAddr, 2, guestStruct(rcs, sizeof(rcs)), andv, xorv });
                        ((void (*)(const void*, S32, const void*, U32, U32))fn(table, SOLID_RECTS))(dst.hostInfo, 2, rcs, andv, xorv);
                        same(dst, "solid_rects");

                        // copy_rect in place, overlapping: one region, merged over both rects, the overlap handling kept.
                        const Point shifts[4] = { { 1, 0 }, { 0, 1 }, { 2, 3 }, { -1, 0 } };
                        Point d = shifts[(left + width) & 3];
                        if (left + d.x < 0) d.x = 1;
                        const Point pt = { left + d.x, top + d.y };
                        U32 overlap = d.y > 0 ? OVERLAP_ABOVE : d.x > 0 ? OVERLAP_LEFT : OVERLAP_RIGHT;
                        U32 rop = width & 1 ? R2_COPYPEN : R2_XORPEN;
                        mustRun(table, COPY_RECT, { dst.infoAddr, grc, dst.infoAddr, guestStruct(&pt, sizeof(pt)), rop, overlap });
                        ((void (*)(const void*, const void*, const void*, const void*, S32, S32))fn(table, COPY_RECT))(dst.hostInfo, &rc, dst.hostInfo, &pt, rop, overlap);
                        same(dst, "copy_rect in place");

                        // copy_rect from another bitmap: a source that's written back or at the wrong row shows here.
                        mustRun(table, COPY_RECT, { dst.infoAddr, grc, other.infoAddr, guestStruct(&pt, sizeof(pt)), R2_COPYPEN, 0 });
                        ((void (*)(const void*, const void*, const void*, const void*, S32, S32))fn(table, COPY_RECT))(dst.hostInfo, &rc, other.hostInfo, &pt, R2_COPYPEN, 0);
                        same(dst, "copy_rect");
                        same(other, "copy_rect's source");

                        // mask_rect: a 1 bpp source with its color table next to any destination: two bit depths at once.
                        mustRun(table, MASK_RECT, { dst.infoAddr, grc, oneBit.infoAddr, guestStruct(&pt, sizeof(pt)), R2_COPYPEN });
                        ((void (*)(const void*, const void*, const void*, const void*, S32))fn(table, MASK_RECT))(dst.hostInfo, &rc, oneBit.hostInfo, &pt, R2_COPYPEN);
                        same(dst, "mask_rect");

                        // draw_glyph, the path all text takes: the glyph's color table and funcs are unset in the guest.
                        U8 ranges[17 * 6];
                        for (U8& v : ranges) v = (U8)rng();
                        U32 text = rng();
                        mustRun(table, DRAW_GLYPH, { dst.infoAddr, grc, glyph.infoAddr, guestStruct(&pt, sizeof(pt)), text, guestStruct(ranges, sizeof(ranges)) });
                        ((void (*)(const void*, const void*, const void*, const void*, U32, const void*))fn(table, DRAW_GLYPH))(dst.hostInfo, &rc, glyph.hostInfo, &pt, text, ranges);
                        same(dst, "draw_glyph");

                        // blend_rects: the source's rect moved by the offset, from a 32 bpp bitmap.
                        const Point offset = { 2, 1 };
                        Blend blend = { 0, 0, (U8)(width & 1 ? 128 : 255), (U8)(width & 2 ? 1 : 0) };
                        U32 blendBits;
                        ::memcpy(&blendBits, &blend, 4);
                        mustRun(table, BLEND_RECTS, { dst.infoAddr, 2, guestStruct(rcs, sizeof(rcs)), argb.infoAddr, guestStruct(&offset, sizeof(offset)), blendBits });
                        ((void (*)(const void*, S32, const void*, const void*, const void*, Blend))fn(table, BLEND_RECTS))(dst.hostInfo, 2, rcs, argb.hostInfo, &offset, blend);
                        same(dst, "blend_rects");

                        // stretch_row/shrink_row, forward and mirrored: the host works out how far the rarer side steps,
                        // from the same error terms; one step too few reads or writes outside the host copy.
                        for (U32 slot : { STRETCH_ROW, SHRINK_ROW }) {
                            const S32 inc = width & 1 ? 1 : -1;
                            const StretchParams params = { left - 5, -8, 3, 12, inc, -inc };
                            const Point p1 = { inc > 0 ? left : left + 11, top }, p2 = { inc > 0 ? 25 : 14, top + 1 };
                            U32 mode = width & 2 ? STRETCH_DELETESCANS : STRETCH_ANDSCANS, keep = width & 2 ? 0 : 1;
                            mustRun(table, slot, { dst.infoAddr, guestStruct(&p1, sizeof(p1)), other.infoAddr, guestStruct(&p2, sizeof(p2)), guestStruct(&params, sizeof(params)), mode, keep });
                            ((void (*)(const void*, const void*, const void*, const void*, const void*, S32, S32))fn(table, slot))(dst.hostInfo, &p1, other.hostInfo, &p2, &params, (S32)mode, (S32)keep);
                            same(dst, "stretch_row/shrink_row");
                            same(other, "stretch_row/shrink_row's source");
                        }

                        // get_pixel reads through the same math, and returns through EAX.
                        mustRun(table, GET_PIXEL, { dst.infoAddr, (U32)left, (U32)top });
                        U32 pixel = ((U32 (*)(const void*, S32, S32))fn(table, GET_PIXEL))(dst.hostInfo, left, top);
                        if (EAX != pixel) testFail("get_pixel: %x, not %x, %u bpp", EAX, pixel, tableBpp[table]);

                        // convert_to: a new bitmap the size of the source rect, every row written.
                        for (U32 to : { T_8888, T_24 }) {
                            static Bmp conv;
                            makeBmp(conv, to, width, rc.bottom - rc.top, 1, { 0, 0, width, rc.bottom - rc.top });
                            mustRun(to, CONVERT_TO, { conv.infoAddr, dst.infoAddr, grc, 0 });
                            ((void (*)(void*, const void*, const void*, S32))fn(to, CONVERT_TO))(conv.hostInfo, dst.hostInfo, &rc, 0);
                            same(conv, "convert_to", true); // finding 174: its fast path can fill the padding differently
                            same(dst, "convert_to's source");
                        }
                    }
                }
                // No pixels, only the color table on the host, and a value back.
                for (U32 v : { 0u, 0x123456u, 0xffffffu }) {
                    mustRun(table, COLORREF_TO_PIXEL, { dst.infoAddr, v });
                    U32 want = ((U32 (*)(const void*, U32))fn(table, COLORREF_TO_PIXEL))(dst.hostInfo, v);
                    if (EAX != want) testFail("colorref_to_pixel %x: %x, not %x, %u bpp", v, EAX, want, tableBpp[table]);
                    mustRun(table, PIXEL_TO_COLORREF, { dst.infoAddr, v & 1 });
                    want = ((U32 (*)(const void*, U32))fn(table, PIXEL_TO_COLORREF))(dst.hostInfo, v & 1);
                    if (EAX != want) testFail("pixel_to_colorref %x: %x, not %x, %u bpp", v, EAX, want, tableBpp[table]);
                }
            }
        }
    }
    // stretch_row/shrink_row with the terms win32u computes for every pair of sizes, the smaller side's last pixel on the
    // bitmap's last column: the host's move count, worked out without stepping, one too high leaves it to the guest and
    // one too low misses that pixel.
    next = heapStart;
    static Bmp big, small;
    makeBmp(big, T_8888, W, 2, 1, { 0, 0, W, 2 });
    makeBmp(small, T_8888, W, 2, -1, { 0, 0, W, 2 });
    const U32 bigStart = next;
    for (S32 major = 1; major <= W; major++) {
        for (S32 minor = 1; minor <= major; minor++) {
            next = bigStart;
            const StretchParams params = { 2 * minor - major, 2 * minor - 2 * major, 2 * minor, (U32)major, 1, 1 };
            const Point pBig = { 0, 0 }, pSmall = { W - minor, 1 };
            for (U32 slot : { STRETCH_ROW, SHRINK_ROW }) {
                Bmp& dst = slot == STRETCH_ROW ? big : small;
                Bmp& src = slot == STRETCH_ROW ? small : big;
                const Point& pDst = slot == STRETCH_ROW ? pBig : pSmall;
                const Point& pSrc = slot == STRETCH_ROW ? pSmall : pBig;
                mustRun(T_8888, slot, { dst.infoAddr, guestStruct(&pDst, sizeof(pDst)), src.infoAddr, guestStruct(&pSrc, sizeof(pSrc)), guestStruct(&params, sizeof(params)), STRETCH_DELETESCANS, 0 });
                ((void (*)(const void*, const void*, const void*, const void*, const void*, S32, S32))fn(T_8888, slot))(dst.hostInfo, &pDst, src.hostInfo, &pSrc, &params, (S32)STRETCH_DELETESCANS, 0);
                same(dst, "stretch_row/shrink_row at the edge");
            }
        }
    }
    // A new DIB section's memory: mapped, never written, so no RAM yet. It gets some, as the guest's first write would.
    Rect whole = { 0, 0, 16, 4 };
    next = heapStart;
    U32 fresh = memory->mmap(cpu->thread, 0, 0x2000, K_PROT_READ | K_PROT_WRITE, K_MAP_PRIVATE | K_MAP_ANONYMOUS, -1, 0);
    static Bmp untouched;
    makeBmp(untouched, T_8888, 16, 4, 1, whole);
    std::fill(untouched.host.begin(), untouched.host.end(), 0);
    untouched.guest = fresh;
    untouched.writeInfo(whole);
    mustRun(T_8888, SOLID_RECTS, { untouched.infoAddr, 1, guestStruct(&whole, sizeof(whole)), 0, 0x11223344 });
    ((void (*)(const void*, S32, const void*, U32, U32))vnbox_dibdrv_host_fn(T_8888, SOLID_RECTS))(untouched.hostInfo, 1, &whole, 0, 0x11223344);
    same(untouched, "a DIB section's first draw");
    memory->unmap(fresh, 0x2000);
}

// Calls the host can't run on copies stay with the guest, with nothing written. Catches a guard that lets a call through
// and draws the wrong thing or faults the emulator, where the guest function was right.
void testHostDibdrvDeclines() {
    setup();
    static Bmp a;
    for (int sign : { 1, -1 }) {
        const Rect whole = { 0, 0, 16, 8 };
        makeBmp(a, T_8888, 16, 8, sign, whole);
        const Point zero = { 0, 0 };
        const U32 gzero = guestStruct(&zero, sizeof(zero));
        auto copyFromRows = [&](S32 rows) { // a second "bitmap" on the same memory, its rows start `rows` further on
            const Rect rc = { 0, 0, 16, 2 }; // full rows of both: the copies must not see each other's writes when they overlap
            U8 info[DI_SIZE], hostInfo[DI_SIZE];
            memory->memcpy(info, a.infoAddr, DI_SIZE);
            Bmp::put(info, DI_BITS, a.guestBits(rows));
            ::memcpy(hostInfo, a.hostInfo, DI_SIZE);
            Bmp::put(hostInfo, DI_BITS, (U32)(uintptr_t)a.hostBits(rows));
            bool ran = run(T_8888, COPY_RECT, { a.infoAddr, guestStruct(&rc, sizeof(rc)), guestStruct(info, DI_SIZE), gzero, R2_COPYPEN, 0 });
            if (ran) ((void (*)(const void*, const void*, const void*, const void*, S32, S32))vnbox_dibdrv_host_fn(T_8888, COPY_RECT))(a.hostInfo, &rc, hostInfo, &zero, R2_COPYPEN, 0);
            return ran;
        };
        // lo()/hi(), both stride signs: rows that just meet are separate, one shared row is not.
        if (!copyFromRows(2)) testFail("bitmaps side by side left to the guest, stride %d", a.stride);
        same(a, "copy between bitmaps side by side");
        if (copyFromRows(1)) testFail("overlapping bitmaps taken, stride %d", a.stride);
        if (copyFromRows(-1)) testFail("overlapping bitmaps taken, stride %d (source first)", a.stride);
        same(a, "declined copy");

        auto solid = [&](const Rect& rc) { return run(T_8888, SOLID_RECTS, { a.infoAddr, 1, guestStruct(&rc, sizeof(rc)), 0, 0x55555555 }); };
        if (solid({ 0, 0, 17, 1 }) || solid({ -1, 0, 4, 1 }) || solid({ 0, 7, 4, 9 }) || solid({ 0, -1, 4, 1 })) testFail("rect outside the bitmap taken");
        // the second of two rects is bad: the first may not have been drawn
        const Rect rcs[2] = { { 0, 0, 2, 2 }, { 0, 0, 99, 2 } };
        if (run(T_8888, SOLID_RECTS, { a.infoAddr, 2, guestStruct(rcs, sizeof(rcs)), 0, 0x55555555 })) testFail("rects with a bad one taken");
        same(a, "declined solid_rects");
    }
    // pixels not mapped: nothing can be copied
    const Rect rc = { 0, 0, 4, 1 };
    const U32 grc = guestStruct(&rc, sizeof(rc));
    U8 info[DI_SIZE];
    memory->memcpy(info, a.infoAddr, DI_SIZE);
    Bmp::put(info, DI_BITS, 0x50000000);
    Bmp::put(info, DI_STRIDE, 64);
    U32 unmapped = guestStruct(info, DI_SIZE);
    if (run(T_8888, SOLID_RECTS, { unmapped, 1, grc, 0, 1 })) testFail("unmapped pixels taken");
    // a debug trap: the host's writes wouldn't fire a watchpoint, and the guest function does the same job
    cpu->debugTrapActive = true;
    bool ran = run(T_8888, SOLID_RECTS, { a.infoAddr, 1, grc, 0, 1 });
    cpu->debugTrapActive = false;
    if (ran) testFail("call taken under a debug trap");
    same(a, "calls left to the guest");
}

#endif
