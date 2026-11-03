/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "boxedwine.h"
#include "vnboxparam.h"

// Host DIB primitives: GDI drawing into DIBs (text, fills, blits, alpha blends, stretches) ends in win32u.so's dibdrv
// primitive functions, the per-pixel loops (antialiased glyph blending: 3 divisions per pixel). jp.zip's win32u.so
// (vn/wine/win32u-dibdrv-export.patch) exports its 9 primitive_funcs tables as vnbox_dibdrv_funcs; the hosted entries
// of those tables get a head op (hostLibcOp, the native libc mechanism in hosthooks.cpp) and run the same functions
// compiled as host wasm (lib/wine-dibdrv: Wine 11.0's primitives.c, unchanged), so the output is the same bytes.
//
// A call is run on host copies: each DIB argument's dib_info (i386 layout, the same as wasm32's) with its bits
// pointer moved to a host buffer that holds just the rows and bytes the call touches (rect.left/top shifted to match,
// keeping each pixel's position within its byte/DWORD and the 24 bpp DWORD triplet), copied in, run, copied back.
// Two arguments on the same bits share one buffer (in-place blits keep their overlap handling); separate bitmaps
// whose touched memory overlaps, file pages not loaded yet, code pages, a debug trap, or anything outside the bitmap
// leave the call to the guest function, as do the primitives not hosted here (lines, pattern brushes, rop masks,
// halftone). Multi-rect calls (solid_rects, blend_rects) are checked rect by rect first, then run one rect at a time.
// A win32u.so without the export (the root fs's) gets nothing hooked. nativeDibdrv=0 (URL) turns it off.
// shortcut: another guest thread writing the same bytes during a hosted call has its write undone by the copy back
// (bytes of the touched rows the primitive itself doesn't change are copied back too); Wine locks GDI objects around
// drawing, so only a game writing into a DIB section's bits while drawing into it from another thread would see it.
#include "../../softmmu/kmemory_soft.h"
#include "../../softmmu/soft_mmu.h"
#include "../../softmmu/soft_ram.h"

enum : U32 {
    DIB_SOLID_RECTS, DIB_SOLID_LINE, DIB_PATTERN_RECTS, DIB_COPY_RECT, DIB_BLEND_RECTS, DIB_GRADIENT_RECT, DIB_MASK_RECT,
    DIB_DRAW_GLYPH, DIB_DRAW_SUBPIXEL_GLYPH, DIB_GET_PIXEL, DIB_COLORREF_TO_PIXEL, DIB_PIXEL_TO_COLORREF, DIB_CONVERT_TO,
    DIB_CREATE_ROP_MASKS, DIB_CREATE_DITHER_MASKS, DIB_STRETCH_ROW, DIB_SHRINK_ROW, DIB_HALFTONE, DIB_SLOTS
};
static const bool dibHosted[DIB_SLOTS] = {
    true, false, false, true, true, true, true, true, true, true, true, true, true, false, false, true, true, false
};
static const char* const dibSlotNames[DIB_SLOTS] = {
    "solid_rects", "solid_line", "pattern_rects", "copy_rect", "blend_rects", "gradient_rect", "mask_rect", "draw_glyph",
    "draw_subpixel_glyph", "get_pixel", "colorref_to_pixel", "pixel_to_colorref", "convert_to", "create_rop_masks",
    "create_dither_masks", "stretch_row", "shrink_row", "halftone"
};
static const U32 DIB_TABLES = 9; // funcs_8888, _32, _24, _555, _16, _8, _4, _1, _null (the export's order)

struct DibEntry { U32 offset; U32 vaddr; U8 table, slot; };
static std::vector<DibEntry> dibEntries;
static U32 dibTableVaddr[DIB_TABLES];
static BString dibPath;
static std::atomic<int> dibState; // 0: no win32u.so mapped yet, 1: ready (set after the table), -1: off or unusable
static U32 dibCalls[DIB_SLOTS], dibGuest[DIB_SLOTS];
static const char* dibWhy; // why the last call was left to the guest (logged once per primitive)

#ifdef BOXEDWINE_HOST_DIBDRV
extern "C" void (*vnbox_dibdrv_host_fn(unsigned table, unsigned slot))(void); // lib/wine-dibdrv/host.c
extern "C" const void* vnbox_dibdrv_host_table(unsigned table);

static U32 dibElf16(const U8* p) { return p[0] | (p[1] << 8); }
static U32 dibElf32(const U8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((U32)p[3] << 24); }

// Reads vnbox_dibdrv_funcs from .dynsym, then the tables it points at from the file (i386 REL: a pointer's link
// address is stored in place), into file offsets of the hosted functions.
static bool hostDibdrvParse(KFile* file) {
    auto read = [file](U64 offset, std::vector<U8>& buf, U32 len) {
        buf.resize(len);
        return file->preadNative(buf.data(), (S64)offset, len) == len;
    };
    std::vector<U8> eh, ph, sh, syms, names, buf;
    if (!read(0, eh, 52) || memcmp(eh.data(), "\x7f" "ELF\x01\x01", 6) || dibElf16(&eh[18]) != 3) return false;
    U32 phoff = dibElf32(&eh[28]), phentsize = dibElf16(&eh[42]), phnum = dibElf16(&eh[44]);
    U32 shoff = dibElf32(&eh[32]), shentsize = dibElf16(&eh[46]), shnum = dibElf16(&eh[48]);
    if (phentsize != 32 || shentsize != 40 || !read(phoff, ph, phnum * 32) || !shnum || !read(shoff, sh, shnum * 40)) return false;
    // vaddr -> file offset through the PT_LOAD segments; exec: must be in an executable one
    auto fileOffset = [&](U32 vaddr, bool exec, U32 len, U32& out) {
        for (U32 i = 0; i < phnum; i++) {
            const U8* p = &ph[i * 32];
            U32 offset = dibElf32(p + 4), va = dibElf32(p + 8), filesz = dibElf32(p + 16), flags = dibElf32(p + 24);
            if (dibElf32(p) != 1 || vaddr < va || (U64)vaddr + len > (U64)va + filesz || (exec && !(flags & 1))) continue;
            out = vaddr - va + offset;
            return true;
        }
        return false;
    };
    U32 exportVaddr = 0;
    for (U32 i = 0; i < shnum && !exportVaddr; i++) {
        const U8* s = &sh[i * 40];
        U32 link = dibElf32(s + 24);
        if (dibElf32(s + 4) != 11 || link >= shnum) continue; // SHT_DYNSYM
        const U8* str = &sh[link * 40];
        if (!read(dibElf32(s + 16), syms, dibElf32(s + 20)) || !read(dibElf32(str + 16), names, dibElf32(str + 20))) return false;
        for (U32 off = 0; off + 16 <= syms.size(); off += 16) {
            const U8* sym = &syms[off];
            U32 name = dibElf32(sym);
            if ((sym[12] & 0xf) != 1 || dibElf32(sym + 8) != DIB_TABLES * 4 || name >= names.size()) continue; // STT_OBJECT
            if (!memchr(&names[name], 0, names.size() - name) || strcmp((const char*)&names[name], "vnbox_dibdrv_funcs")) continue;
            exportVaddr = dibElf32(sym + 4);
            break;
        }
    }
    U32 offset;
    if (!exportVaddr || !fileOffset(exportVaddr, false, DIB_TABLES * 4, offset) || !read(offset, buf, DIB_TABLES * 4)) return false;
    for (U32 t = 0; t < DIB_TABLES; t++) dibTableVaddr[t] = dibElf32(&buf[t * 4]);
    std::vector<U8> table;
    for (U32 t = 0; t < DIB_TABLES - 1; t++) { // funcs_null's functions do nothing: not worth a head op
        if (!fileOffset(dibTableVaddr[t], false, DIB_SLOTS * 4, offset) || !read(offset, table, DIB_SLOTS * 4)) return false;
        for (U32 slot = 0; slot < DIB_SLOTS; slot++) {
            U32 vaddr = dibElf32(&table[slot * 4]);
            if (!fileOffset(vaddr, true, 1, offset)) return false;
            if (!dibHosted[slot]) continue;
            bool seen = false;
            for (const DibEntry& e : dibEntries) seen |= e.offset == offset;
            if (!seen) dibEntries.push_back({ offset, vaddr, (U8)t, (U8)slot });
        }
    }
    return !dibEntries.empty();
}

// Called by hostLibcMapped for every file mapping: the first win32u.so mapped loads the table. offsets gets the
// file offsets to hook.
bool hostDibdrvMapped(KFile* file, std::vector<U32>& offsets) {
    if (dibState || !file->openFile->node->path.endsWith("/win32u.so")) return false;
    const BString& path = file->openFile->node->path;
#ifdef __EMSCRIPTEN__
    if (vnboxUrlIs("nativeDibdrv", "0")) {
        dibState = -1;
        klog_fmt("[vnbox] native dibdrv off (nativeDibdrv=0)");
        return false;
    }
#endif
    bool ok = hostDibdrvParse(file);
    if (!ok) dibEntries.clear();
    for (const DibEntry& e : dibEntries) offsets.push_back(e.offset);
    if (ok) dibPath = path;
    dibState = ok ? 1 : -1;
    klog_fmt("[vnbox] native dibdrv: %s, %d functions%s", path.c_str(), (int)dibEntries.size(), ok ? "" : " (no vnbox_dibdrv_funcs export)");
    return ok;
}

// Entry index + 1 when offset (in the file `path`) starts a hosted primitive, else 0.
U32 hostDibdrvAt(const BString& path, U32 offset) {
    if (dibState != 1 || path != dibPath) return 0;
    for (size_t i = 0; i < dibEntries.size(); i++) {
        if (dibEntries[i].offset == offset) return (U32)i + 1;
    }
    return 0;
}

#ifdef __TEST
// testHostDibdrv.cpp stands in for hostDibdrvParse: a table's link address and a hosted primitive; its hostDibdrvRun index.
U32 hostDibdrvTestEntry(U32 table, U32 slot, U32 tableVaddr, U32 vaddr) {
    dibTableVaddr[table] = tableVaddr;
    dibEntries.push_back({ 0, vaddr, (U8)table, (U8)slot });
    return (U32)dibEntries.size() - 1;
}
#endif

static U8* dibRam(KMemory* memory, U32 address, bool write) {
    MMU& mmu = getMemData(memory)->mmu[address >> K_PAGE_SHIFT];
    if (write ? !mmu.canWriteRam : !mmu.canReadRam) return nullptr;
    return ramPageGet((RamPage)mmu.ramIndex) + (address & K_PAGE_MASK);
}

// Guest [address, address + len) is plain RAM (writable too when write). A readable and writable page with no RAM
// yet (memory never written, like a new DIB section's or a new malloc's, or copy-on-write) gets it by writing one of
// its bytes back, as the guest's own first write would; file pages (loading one may suspend) and code are left alone.
static bool dibRange(KMemory* memory, U32 address, U32 len, bool write) {
    if (!len) return true;
    if ((U64)address + len > 0x100000000ull) return false;
    for (U32 page = address >> K_PAGE_SHIFT; page <= (address + len - 1) >> K_PAGE_SHIFT; page++) {
        if (dibRam(memory, page << K_PAGE_SHIFT, write)) continue;
        PageType type = getMemData(memory)->mmu[page].getPageType();
        if (type == PageType::File || type == PageType::Code || !memory->canRead(page) || !memory->canWrite(page)) return false;
        U32 a = page << K_PAGE_SHIFT;
        memory->writeb(a, memory->readb(a));
        if (!dibRam(memory, a, write)) return false;
    }
    return true;
}

// Copies between guest [address, address + len) and host; the caller checked the range.
static void dibCopy(KMemory* memory, U32 address, U8* host, U32 len, bool toGuest) {
    while (len) {
        U32 n = std::min(len, K_PAGE_SIZE - (address & K_PAGE_MASK));
        U8* p = dibRam(memory, address, toGuest);
        if (toGuest) ::memcpy(p, host, n); else ::memcpy(host, p, n);
        address += n;
        host += n;
        len -= n;
    }
}

static bool dibRead(KMemory* memory, U32 address, void* out, U32 len) {
    if (!address || !dibRange(memory, address, len, false)) return false;
    dibCopy(memory, address, (U8*)out, len, false);
    return true;
}

struct DibRect { S32 left, top, right, bottom; };
struct DibPoint { S32 x, y; };
struct DibBlend { U8 op, flags, constantAlpha, alphaFormat; }; // BLENDFUNCTION, passed by value as the host C code expects

// The guest's i386 dib_info (100 bytes, the same layout host.c checks for wasm32), fields read through these offsets.
static const U32 DI_BITCOUNT = 0, DI_WIDTH = 4, DI_HEIGHT = 8, DI_RECT = 16, DI_STRIDE = 32, DI_BITS = 36, DI_COLORS = 88,
    DI_COLORCOUNT = 92, DI_FUNCS = 96, DI_SIZE = 100;
static S32 diGet(const U8* d, U32 offset) { S32 v; memcpy(&v, d + offset, 4); return v; }
static void diSet(U8* d, U32 offset, S32 v) { memcpy(d + offset, &v, 4); }

// The bytes of a bitmap a call touches: absolute rows [y0, y1) (from the bitmap's top), bytes [b0, b1) of each.
struct DibRegion {
    U32 bits; S32 stride; U32 bpp;
    S32 y0, y1; U32 b0, b1;
    bool write;
    U8* buf; U32 hostStride;
    U64 lo() const { return (U64)((S64)bits + (S64)(stride > 0 ? y0 : y1 - 1) * stride) + b0; }
    U64 hi() const { return (U64)((S64)bits + (S64)(stride > 0 ? y1 - 1 : y0) * stride) + b1; }
};

struct DibArg {
    alignas(4) U8 info[DI_SIZE]; // guest bytes, then the host copy's
    U32 colors[256]; // RGBQUAD
    DibRect touched; // in the dib's own coordinates (relative to its rect), as the primitive uses them
    bool write, hasBits, colorTable; // colorTable false: the primitive never reads it (a glyph's is left unset)
    U32 region;
};

// One call of a primitive on host copies: add the DIB arguments, then prepare(), run(fn), finish().
struct DibCall {
    KMemory* memory;
    U32 baseAddress; // guest load base of win32u.so (table pointers are base + link address)
    DibArg args[2];
    U32 argCount = 0;
    DibRegion regions[2];
    U32 regionCount = 0;
    std::vector<U8>* arena;

    // A DIB argument: touched (dib coordinates) is what the call reads, and writes when write. hasBits false: no
    // pixels are touched (colorref_to_pixel), only the dib_info and its color table.
    DibArg* add(U32 guest, const DibRect& touched, bool write, bool hasBits = true, bool colorTable = true) {
        DibArg& a = args[argCount++];
        if (!dibRead(memory, guest, a.info, DI_SIZE)) return dibWhy = "dib_info not RAM", nullptr;
        a.touched = touched;
        a.write = write;
        a.hasBits = hasBits;
        a.colorTable = colorTable;
        return &a;
    }

    bool prepare() {
        for (U32 i = 0; i < argCount; i++) {
            DibArg& a = args[i];
            const U8* d = a.info;
            S32 bpp = diGet(d, DI_BITCOUNT), stride = diGet(d, DI_STRIDE), width = diGet(d, DI_WIDTH), height = diGet(d, DI_HEIGHT);
            if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return dibWhy = "bit count", false;
            U32 colors = (U32)diGet(d, DI_COLORS), colorCount = (U32)diGet(d, DI_COLORCOUNT);
            if (!a.colorTable) {
                diSet(a.info, DI_COLORS, 0);
            } else if (colors) {
                if (colorCount > 256 || !dibRead(memory, colors, a.colors, colorCount * 4)) return dibWhy = "color table", false;
                diSet(a.info, DI_COLORS, (S32)(uintptr_t)a.colors);
            }
            U32 funcs = (U32)diGet(d, DI_FUNCS);
            S32 table = -1;
            for (U32 t = 0; t < DIB_TABLES; t++) {
                if (funcs == baseAddress + dibTableVaddr[t]) table = (S32)t;
            }
            // not one of the tables (a glyph's dib_info leaves it unset): null, which compares unequal to every table
            // as the guest's value does
            diSet(a.info, DI_FUNCS, table < 0 ? 0 : (S32)(uintptr_t)vnbox_dibdrv_host_table((U32)table));
            if (!a.hasBits) {
                diSet(a.info, DI_BITS, 0);
                continue;
            }
            U32 bits = (U32)diGet(d, DI_BITS);
            S64 left = (S64)diGet(d, DI_RECT) + a.touched.left, right = (S64)diGet(d, DI_RECT) + a.touched.right;
            S64 top = (S64)diGet(d, DI_RECT + 4) + a.touched.top, bottom = (S64)diGet(d, DI_RECT + 4) + a.touched.bottom;
            if (!bits || !stride || (stride & 3) || left < 0 || top < 0 || left >= right || top >= bottom || right > width || bottom > height) return dibWhy = "outside the bitmap", false;
            // b0 keeps each pixel's position in its DWORD (and 24 bpp's 4-pixel triplet): a multiple of 4 (12) bytes
            U64 b0 = (U64)left * bpp / 8, b1 = ((U64)right * bpp + 7) / 8;
            b0 = bpp == 24 ? b0 / 12 * 12 : b0 & ~3ull;
            b1 = (b1 + 3) & ~3ull;
            if (b1 > (U64)std::abs((S64)stride)) return dibWhy = "outside the stride", false;
            U32 r = 0;
            for (; r < regionCount; r++) {
                DibRegion& g = regions[r];
                if (g.bits == bits && g.stride == stride && g.bpp == (U32)bpp) break;
            }
            if (r == regionCount) {
                regions[regionCount++] = { bits, stride, (U32)bpp, (S32)top, (S32)bottom, (U32)b0, (U32)b1, a.write, nullptr, 0 };
            } else {
                DibRegion& g = regions[r];
                g.y0 = std::min(g.y0, (S32)top);
                g.y1 = std::max(g.y1, (S32)bottom);
                g.b0 = std::min(g.b0, (U32)b0);
                g.b1 = std::max(g.b1, (U32)b1);
                g.write |= a.write;
            }
            a.region = r;
        }
        // separate bitmaps over the same memory: the copies would not see each other's writes
        for (U32 i = 0; i < regionCount; i++) {
            for (U32 j = i + 1; j < regionCount; j++) {
                const DibRegion& x = regions[i];
                const DibRegion& y = regions[j];
                if ((x.write || y.write) && x.lo() < y.hi() && y.lo() < x.hi()) return dibWhy = "bitmaps overlap", false;
            }
        }
        U32 total = 0;
        for (U32 i = 0; i < regionCount; i++) {
            DibRegion& g = regions[i];
            if (g.hi() > 0x100000000ull) return dibWhy = "wraps", false;
            for (S32 y = g.y0; y < g.y1; y++) {
                if (!dibRange(memory, (U32)(g.bits + (S64)y * g.stride + g.b0), g.b1 - g.b0, g.write)) return dibWhy = "pixels not plain RAM", false;
            }
            g.hostStride = g.b1 - g.b0;
            total += g.hostStride * (U32)(g.y1 - g.y0);
        }
        if (arena->size() < total) arena->resize(total);
        U32 at = 0;
        for (U32 i = 0; i < regionCount; i++) {
            regions[i].buf = arena->data() + at;
            at += regions[i].hostStride * (U32)(regions[i].y1 - regions[i].y0);
        }
        for (U32 i = 0; i < argCount; i++) {
            DibArg& a = args[i];
            if (!a.hasBits) continue;
            const DibRegion& g = regions[a.region];
            S32 p0 = (S32)((U64)g.b0 * 8 / g.bpp); // first pixel of the host rows
            S32 dx = -p0, dy = -g.y0;
            for (U32 k = 0; k < 4; k++) diSet(a.info, DI_RECT + k * 4, diGet(a.info, DI_RECT + k * 4) + (k & 1 ? dy : dx));
            S32 hostStride = g.stride > 0 ? (S32)g.hostStride : -(S32)g.hostStride;
            U8* first = g.stride > 0 ? g.buf : g.buf + (size_t)g.hostStride * (g.y1 - g.y0 - 1); // host row of y0
            diSet(a.info, DI_STRIDE, hostStride);
            diSet(a.info, DI_BITS, (S32)(uintptr_t)first);
        }
        return true;
    }

    // host row index of absolute row y, as laid out in prepare()
    U8* hostRow(const DibRegion& g, S32 y) {
        S32 i = g.stride > 0 ? y - g.y0 : g.y1 - 1 - y;
        return g.buf + (size_t)i * g.hostStride;
    }

    void copyIn() {
        for (U32 i = 0; i < regionCount; i++) {
            DibRegion& g = regions[i];
            for (S32 y = g.y0; y < g.y1; y++) dibCopy(memory, (U32)(g.bits + (S64)y * g.stride + g.b0), hostRow(g, y), g.hostStride, false);
        }
    }

    void copyOut() {
        for (U32 i = 0; i < regionCount; i++) {
            DibRegion& g = regions[i];
            if (!g.write) continue;
            for (S32 y = g.y0; y < g.y1; y++) dibCopy(memory, (U32)(g.bits + (S64)y * g.stride + g.b0), hostRow(g, y), g.hostStride, true);
        }
    }
};

static DibRect dibRectAt(const DibPoint& p, S32 w, S32 h) { return { p.x, p.y, p.x + w, p.y + h }; }

// The x range of one row stretch_row/shrink_row read or write: steps + 1 pixels from start, inc apart.
static DibRect dibRowSpan(const DibPoint& start, S32 steps, S32 inc) {
    S64 end = (S64)start.x + (S64)steps * inc;
    S64 lo = std::min((S64)start.x, end), hi = std::max((S64)start.x, end) + 1;
    if (lo < INT32_MIN || hi > INT32_MAX) return { 0, 0, 0, 0 };
    return { (S32)lo, start.y, (S32)hi, start.y + 1 };
}

static bool dibGuestCall(U32 slot) {
    dibGuest[slot]++;
    if (dibGuest[slot] == 1) klog_fmt("[vnbox] native dibdrv: a %s call runs as guest code (%s; after %u host calls)", dibSlotNames[slot], dibWhy ? dibWhy : "arguments", dibCalls[slot]);
    dibWhy = nullptr;
    return false;
}

// Runs the hosted primitive `index` (hostDibdrvAt - 1) for the call the CPU is at (i386 cdecl), sets EAX and returns
// like its `ret`; false (nothing changed) leaves it to the guest function.
bool hostDibdrvRun(CPU* cpu, U32 index) {
    if (index >= dibEntries.size()) return false;
    const DibEntry& e = dibEntries[index];
    KMemory* memory = cpu->memory;
    U32 slot = e.slot;
    if (cpu->getDirection() < 0 || cpu->debugTrapActive || !cpu->isBig() || cpu->stackMask != 0xFFFFFFFF ||
        cpu->seg[CS].address || cpu->seg[DS].address || cpu->seg[ES].address || cpu->seg[SS].address) return dibGuestCall(slot);
    U32 a[7];
    if (!dibRead(memory, ESP + 4, a, sizeof(a))) return dibGuestCall(slot);
    U32 eip = cpu->seg[CS].address + cpu->eip.u32;
    static thread_local std::vector<U8> arena;
    DibCall call;
    call.memory = memory;
    call.baseAddress = eip - e.vaddr;
    call.arena = &arena;
    void (*fn)(void) = vnbox_dibdrv_host_fn(e.table, slot);
    U32 result = EAX;
    DibRect rc;
    DibPoint pt, pt2;
    switch (slot) {
    case DIB_DRAW_GLYPH:
    case DIB_DRAW_SUBPIXEL_GLYPH: { // (dst, rc, glyph, origin, text_pixel, ranges / gamma_ramp)
        U8 extra[516];
        U32 extraLen = slot == DIB_DRAW_GLYPH ? 17 * 6 : 516;
        if (!dibRead(memory, a[1], &rc, 16) || !dibRead(memory, a[3], &pt, 8)) return dibGuestCall(slot);
        if (a[5] && !dibRead(memory, a[5], extra, extraLen)) return dibGuestCall(slot);
        if (!call.add(a[0], rc, true) || !call.add(a[2], dibRectAt(pt, rc.right - rc.left, rc.bottom - rc.top), false, true, false) || !call.prepare()) return dibGuestCall(slot);
        call.copyIn();
        ((void (*)(const void*, const void*, const void*, const void*, U32, const void*))fn)(call.args[0].info, &rc, call.args[1].info, &pt, a[4], a[5] ? extra : nullptr);
        call.copyOut();
        break;
    }
    case DIB_COPY_RECT: // (dst, rc, src, origin, rop2, overlap)
    case DIB_MASK_RECT: // (dst, rc, src, origin, rop2)
        if (!dibRead(memory, a[1], &rc, 16) || !dibRead(memory, a[3], &pt, 8)) return dibGuestCall(slot);
        if (!call.add(a[0], rc, true) || !call.add(a[2], dibRectAt(pt, rc.right - rc.left, rc.bottom - rc.top), false) || !call.prepare()) return dibGuestCall(slot);
        call.copyIn();
        if (slot == DIB_COPY_RECT) ((void (*)(const void*, const void*, const void*, const void*, S32, S32))fn)(call.args[0].info, &rc, call.args[1].info, &pt, (S32)a[4], (S32)a[5]);
        else ((void (*)(const void*, const void*, const void*, const void*, S32))fn)(call.args[0].info, &rc, call.args[1].info, &pt, (S32)a[4]);
        call.copyOut();
        break;
    case DIB_SOLID_RECTS: // (dib, num, rc, and, xor)
    case DIB_BLEND_RECTS: { // (dst, num, rc, src, offset, blend)
        S32 num = (S32)a[1];
        if (num < 0 || num > 0x10000) return dibGuestCall(slot);
        std::vector<DibRect> rects(num);
        if (num && !dibRead(memory, a[2], rects.data(), num * 16)) return dibGuestCall(slot);
        if (slot == DIB_BLEND_RECTS && !dibRead(memory, a[4], &pt, 8)) return dibGuestCall(slot);
        // every rect is checked before the first is drawn, then each is set up again just before it runs (the same
        // checks on the same memory, so it can't fail then)
        auto setup = [&](DibCall& c, S32 i) {
            const DibRect& r = rects[i];
            c.memory = memory;
            c.baseAddress = call.baseAddress;
            c.arena = &arena;
            return c.add(a[0], r, true) && (slot != DIB_BLEND_RECTS || c.add(a[3], { r.left + pt.x, r.top + pt.y, r.right + pt.x, r.bottom + pt.y }, false)) && c.prepare();
        };
        for (S32 i = 0; i < num; i++) {
            DibCall c;
            if (!setup(c, i)) return dibGuestCall(slot);
        }
        DibBlend blend;
        memcpy(&blend, &a[5], 4);
        for (S32 i = 0; i < num; i++) {
            DibCall c;
            setup(c, i);
            c.copyIn();
            if (slot == DIB_SOLID_RECTS) ((void (*)(const void*, S32, const void*, U32, U32))fn)(c.args[0].info, 1, &rects[i], a[3], a[4]);
            else ((void (*)(const void*, S32, const void*, const void*, const void*, DibBlend))fn)(c.args[0].info, 1, &rects[i], c.args[1].info, &pt, blend);
            c.copyOut();
        }
        break;
    }
    case DIB_GRADIENT_RECT: { // (dib, rc, v, mode) -> BOOL
        U8 v[3 * 16];
        if (!dibRead(memory, a[1], &rc, 16) || !dibRead(memory, a[2], v, (a[3] == 2 ? 3 : 2) * 16)) return dibGuestCall(slot); // 2: GRADIENT_FILL_TRIANGLE
        if (!call.add(a[0], rc, true) || !call.prepare()) return dibGuestCall(slot);
        call.copyIn();
        result = (U32)((S32 (*)(const void*, const void*, const void*, S32))fn)(call.args[0].info, &rc, v, (S32)a[3]);
        call.copyOut();
        break;
    }
    case DIB_GET_PIXEL: // (dib, x, y) -> DWORD
        if (!call.add(a[0], { (S32)a[1], (S32)a[2], (S32)a[1] + 1, (S32)a[2] + 1 }, false) || !call.prepare()) return dibGuestCall(slot);
        call.copyIn();
        result = ((U32 (*)(const void*, S32, S32))fn)(call.args[0].info, (S32)a[1], (S32)a[2]);
        break;
    case DIB_COLORREF_TO_PIXEL: // (dib, color) -> DWORD
    case DIB_PIXEL_TO_COLORREF: // (dib, pixel) -> COLORREF
        if (!call.add(a[0], {}, false, false) || !call.prepare()) return dibGuestCall(slot);
        result = ((U32 (*)(const void*, U32))fn)(call.args[0].info, a[1]);
        break;
    case DIB_CONVERT_TO: { // (dst, src, src_rect, dither): dst is a new bitmap the size of src_rect, every row written
        U8 dst[DI_SIZE];
        if (!dibRead(memory, a[2], &rc, 16) || !dibRead(memory, a[0], dst, DI_SIZE)) return dibGuestCall(slot);
        S32 left = diGet(dst, DI_RECT), top = diGet(dst, DI_RECT + 4);
        if (!call.add(a[0], { -left, -top, diGet(dst, DI_WIDTH) - left, diGet(dst, DI_HEIGHT) - top }, true) || !call.add(a[1], rc, false) || !call.prepare()) return dibGuestCall(slot);
        call.copyIn();
        ((void (*)(void*, const void*, const void*, S32))fn)(call.args[0].info, call.args[1].info, &rc, (S32)a[3]);
        call.copyOut();
        break;
    }
    case DIB_STRETCH_ROW:
    case DIB_SHRINK_ROW: { // (dst, dst_start, src, src_start, params, mode, keep_dst)
        S32 params[6]; // err_start, err_add_1, err_add_2, length, dst_inc, src_inc
        if (!dibRead(memory, a[1], &pt, 8) || !dibRead(memory, a[3], &pt2, 8) || !dibRead(memory, a[4], params, sizeof(params))) return dibGuestCall(slot);
        S32 length = params[3];
        if (length <= 0 || length > 0x1000000) return dibGuestCall(slot);
        // one side steps every pixel (dst for stretch_row, src for shrink_row), the other when err > 0; a step after
        // the last pixel isn't read or written
        S32 err = params[0], moves = 0;
        for (S32 i = 0; i < length - 1; i++) {
            if (err > 0) { moves++; err += params[1]; } else err += params[2];
        }
        S32 dstSteps = slot == DIB_STRETCH_ROW ? length - 1 : moves, srcSteps = slot == DIB_STRETCH_ROW ? moves : length - 1;
        if (!call.add(a[0], dibRowSpan(pt, dstSteps, params[4]), true) || !call.add(a[2], dibRowSpan(pt2, srcSteps, params[5]), false) || !call.prepare()) return dibGuestCall(slot);
        call.copyIn();
        ((void (*)(const void*, const void*, const void*, const void*, const void*, S32, S32))fn)(call.args[0].info, &pt, call.args[1].info, &pt2, params, (S32)a[5], (S32)a[6]);
        call.copyOut();
        break;
    }
    default:
        return dibGuestCall(slot);
    }
    dibCalls[slot]++;
    EAX = result;
    cpu->eip.u32 = cpu->pop32();
    return true;
}
#else
bool hostDibdrvMapped(KFile*, std::vector<U32>&) { return false; }
U32 hostDibdrvAt(const BString&, U32) { return 0; }
bool hostDibdrvRun(CPU*, U32) { return false; }
#endif
