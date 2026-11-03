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

// Host glyph metrics: GDI text games measure their text every frame (GetTextExtentExPoint, GetGlyphOutline with
// GGO_METRICS, and ExtTextOut without a dx array measures too), a few hundred emulated instructions per char in
// win32u.so even when every answer is cached: font_lock, the char -> glyph lookup, the linked-font walk, the
// font->gm metrics cache. jp.zip's win32u.so (vn/wine/win32u-glyphmetrics-export.patch) exports vnbox_glyphm
// (font.c: the link addresses of font_GetTextExtentExPoint, font_GetTextExtentExPointI, font_GetGlyphOutline,
// font_lock, font_funcs, and the structure offsets) and vnbox_glyphm_ft (freetype.c: its backend table, the
// offsets of the char_index cache of win32u-glyph-cache.patch and of FT_Face's charmap encoding). Those three
// functions get a head op (hostLibcOp, the native libc mechanism in hosthooks.cpp) that does the call in C++ the
// way get_glyph_outline answers a GGO_METRICS call without a matrix from its cache: char -> glyph through
// char_index (Unicode charmaps; not a symbol or no-encoding font), not a vertical ('@') font, linked fonts walked
// as get_glyph_index_linked does (a child not loaded yet: guest), then font->gm[block][entry] with init set.
//
// Every glyph of the call has to be cached and every guest page read or written plain RAM, else nothing is
// written and the whole call runs as guest code (which fills the caches for the next call). font_lock has to be
// free: a guest thread inside font_lock may be changing these structures (realloc of font->gm), and the guest
// function would wait for it; host code runs between guest instructions of a single-threaded CPU, so it takes
// and drops the lock at once. Multi-threaded builds (guest threads on host threads) don't hook anything.
// A win32u.so without the exports (the root fs's) gets nothing hooked. nativeGlyphMetrics=0 (URL) turns it off.
// shortcut: win32u-glyph-cache.patch's VNBOX_GLYPHSTATS counters don't count the host's char lookups.
#include "../../softmmu/kmemory_soft.h"
#include "../../softmmu/soft_mmu.h"
#include "../../softmmu/soft_ram.h"

enum : U32 { GM_EXTENT, GM_EXTENT_I, GM_GLYPH_OUTLINE, GM_FNS };
static const char* const gmNames[GM_FNS] = { "font_GetTextExtentExPoint", "font_GetTextExtentExPointI", "font_GetGlyphOutline" };

// vnbox_glyphm (version 1), in this order
enum : U32 {
    GI_VERSION, GI_EXTENT, GI_EXTENT_I, GI_GLYPH_OUTLINE, GI_FONT_LOCK, GI_FONT_FUNCS, GI_PHYSDEV_FONT,
    GI_FONT_ENTRY, GI_FONT_GM_SIZE, GI_FONT_GM, GI_FONT_PRIVATE, GI_FONT_CHILDREN, GI_FONT_LF_FACE, GI_FONT_OTM_FAMILY,
    GI_GM_BLOCK, GI_GM_SIZE, GI_GM_GM, GI_GM_ABC, GI_GM_INIT, GI_COUNT
};
// vnbox_glyphm_ft (version 1)
enum : U32 { GF_VERSION, GF_FUNCS, GF_FT_FACE, GF_CHAR_INDEX, GF_CHARMAP, GF_ENCODING, GF_ENC_NONE, GF_ENC_SYMBOL, GF_COUNT };

static U32 gmInfo[GI_COUNT], gmFt[GF_COUNT];
static U32 gmFnOffset[GM_FNS]; // file offsets of the hooked functions
static BString gmPath;
static std::atomic<int> gmState; // 0: no win32u.so mapped yet, 1: ready (set after the tables), -1: off or unusable
static U32 gmCalls[GM_FNS], gmGuest[GM_FNS];
static const char* gmWhy; // why the last call was left to the guest (logged for the first one per function)

#if !defined(BOXEDWINE_MULTI_THREADED)
U32 hostElf16(const U8* p); // hosthooks.cpp
U32 hostElf32(const U8* p);
bool hostFileRead(KFile* file, U64 offset, std::vector<U8>& buf, U32 len);
void hostHookOff(const char* what); // hosthooks.cpp: tells the page a hook pinned to this root fs did not take

// Reads both exports from .dynsym and the file (i386 REL: a pointer's link address is stored in place).
static bool hostGlyphmParse(KFile* file) {
    std::vector<U8> eh, ph, sh, syms, names, buf;
    if (!hostFileRead(file, 0, eh, 52) || memcmp(eh.data(), "\x7f" "ELF\x01\x01", 6) || hostElf16(&eh[18]) != 3) return false;
    U32 phoff = hostElf32(&eh[28]), phentsize = hostElf16(&eh[42]), phnum = hostElf16(&eh[44]);
    U32 shoff = hostElf32(&eh[32]), shentsize = hostElf16(&eh[46]), shnum = hostElf16(&eh[48]);
    if (phentsize != 32 || shentsize != 40 || !hostFileRead(file, phoff, ph, phnum * 32) || !shnum || !hostFileRead(file, shoff, sh, shnum * 40)) return false;
    // vaddr -> file offset through the PT_LOAD segments; exec: must be in an executable one
    auto fileOffset = [&](U32 vaddr, bool exec, U32 len, U32& out) {
        for (U32 i = 0; i < phnum; i++) {
            const U8* p = &ph[i * 32];
            U32 offset = hostElf32(p + 4), va = hostElf32(p + 8), filesz = hostElf32(p + 16), flags = hostElf32(p + 24);
            if (hostElf32(p) != 1 || vaddr < va || (U64)vaddr + len > (U64)va + filesz || (exec && !(flags & 1))) continue;
            out = vaddr - va + offset;
            return true;
        }
        return false;
    };
    U32 infoVaddr = 0, ftVaddr = 0;
    for (U32 i = 0; i < shnum; i++) {
        const U8* s = &sh[i * 40];
        U32 link = hostElf32(s + 24);
        if (hostElf32(s + 4) != 11 || link >= shnum) continue; // SHT_DYNSYM
        const U8* str = &sh[link * 40];
        if (!hostFileRead(file, hostElf32(s + 16), syms, hostElf32(s + 20)) || !hostFileRead(file, hostElf32(str + 16), names, hostElf32(str + 20))) return false;
        for (U32 off = 0; off + 16 <= syms.size(); off += 16) {
            const U8* sym = &syms[off];
            U32 name = hostElf32(sym), size = hostElf32(sym + 8);
            if ((sym[12] & 0xf) != 1 || name >= names.size() || !memchr(&names[name], 0, names.size() - name)) continue; // STT_OBJECT
            const char* n = (const char*)&names[name];
            if (!strcmp(n, "vnbox_glyphm") && size == GI_COUNT * 4) infoVaddr = hostElf32(sym + 4);
            else if (!strcmp(n, "vnbox_glyphm_ft") && size == GF_COUNT * 4) ftVaddr = hostElf32(sym + 4);
        }
        break;
    }
    U32 offset;
    if (!infoVaddr || !ftVaddr || !fileOffset(infoVaddr, false, GI_COUNT * 4, offset) || !hostFileRead(file, offset, buf, GI_COUNT * 4)) return false;
    for (U32 i = 0; i < GI_COUNT; i++) gmInfo[i] = hostElf32(&buf[i * 4]);
    if (!fileOffset(ftVaddr, false, GF_COUNT * 4, offset) || !hostFileRead(file, offset, buf, GF_COUNT * 4)) return false;
    for (U32 i = 0; i < GF_COUNT; i++) gmFt[i] = hostElf32(&buf[i * 4]);
    if (gmInfo[GI_VERSION] != 1 || gmFt[GF_VERSION] != 1 || !gmInfo[GI_GM_BLOCK] || gmInfo[GI_GM_SIZE] < 36) return false;
    for (U32 f = 0; f < GM_FNS; f++) {
        if (!fileOffset(gmInfo[GI_EXTENT + f], true, 1, gmFnOffset[f])) return false;
    }
    return true;
}

// Called by hostLibcMapped for every file mapping: the first win32u.so mapped loads the tables. offsets gets the
// file offsets to hook.
bool hostGlyphmMapped(KFile* file, std::vector<U32>& offsets) {
    if (gmState || !file->openFile->node->path.endsWith("/win32u.so")) return false;
    const BString& path = file->openFile->node->path;
#ifdef __EMSCRIPTEN__
    if (vnboxUrlIs("nativeGlyphMetrics", "0")) {
        gmState = -1;
        klog_fmt("[vnbox] native glyph metrics off (nativeGlyphMetrics=0)");
        return false;
    }
#endif
    bool ok = hostGlyphmParse(file);
    if (ok) {
        for (U32 f = 0; f < GM_FNS; f++) offsets.push_back(gmFnOffset[f]);
        gmPath = path;
    }
    gmState = ok ? 1 : -1;
    klog_fmt("[vnbox] native glyph metrics: %s%s", path.c_str(), ok ? "" : " (no vnbox_glyphm export)");
    if (!ok) hostHookOff("native glyph metrics");
    return ok;
}

// GM_* + 1 when offset (in the file `path`) starts a hooked function, else 0.
U32 hostGlyphmAt(const BString& path, U32 offset) {
    if (gmState != 1 || path != gmPath) return 0;
    for (U32 f = 0; f < GM_FNS; f++) {
        if (gmFnOffset[f] == offset) return f + 1;
    }
    return 0;
}

#ifdef __TEST
// testHostGlyphm.cpp stands in for hostGlyphmParse: both export tables, or none (no win32u.so mapped).
void hostGlyphmTestTables(const U32* info, const U32* ft) {
    if (info) ::memcpy(gmInfo, info, sizeof(gmInfo));
    if (ft) ::memcpy(gmFt, ft, sizeof(gmFt));
    gmState = info ? 1 : 0;
}
#endif

// Guest memory, plain RAM pages only (the soft MMU's own fast-path test); false leaves the call to the guest.
struct GmMem {
    KMemory* memory;
    bool ok = true;

    U8* ram(U32 address, bool write) {
        MMU& mmu = getMemData(memory)->mmu[address >> K_PAGE_SHIFT];
        if (write ? !mmu.canWriteRam : !mmu.canReadRam) return nullptr;
        return ramPageGet((RamPage)mmu.ramIndex) + (address & K_PAGE_MASK);
    }
    bool range(U32 address, U32 len, bool write) {
        if (!len) return true;
        if (!address || (U64)address + len > 0x100000000ull) return false;
        for (U32 page = address >> K_PAGE_SHIFT; page <= (address + len - 1) >> K_PAGE_SHIFT; page++) {
            if (!ram(page << K_PAGE_SHIFT, write)) return false;
        }
        return true;
    }
    // Copies between guest [address, address + len) and host; the caller checked the range.
    void copy(U32 address, U8* host, U32 len, bool toGuest) {
        while (len) {
            U32 n = std::min(len, K_PAGE_SIZE - (address & K_PAGE_MASK));
            U8* p = ram(address, toGuest);
            if (toGuest) ::memcpy(p, host, n); else ::memcpy(host, p, n);
            address += n;
            host += n;
            len -= n;
        }
    }
    bool read(U32 address, void* out, U32 len) {
        if (!range(address, len, false)) return ok = false;
        copy(address, (U8*)out, len, false);
        return true;
    }
    U32 rd(U32 address) { // 0 (and ok false) when not readable
        U32 v = 0;
        if ((address & K_PAGE_MASK) <= K_PAGE_SIZE - 4) {
            if (U8* p = address ? ram(address, false) : nullptr) { ::memcpy(&v, p, 4); return v; }
            ok = false;
            return 0;
        }
        read(address, &v, 4);
        return v;
    }
    U16 rd16(U32 address) {
        U16 v = 0;
        read(address, &v, 2);
        return v;
    }
};

struct GmMetrics { U8 gm[20]; S32 abcA; U32 abcB; S32 abcC; };

struct GmCall {
    GmMem m;
    U32 base; // guest load base of win32u.so

    // freetype_get_glyph_index(font, &glyph, TRUE) through get_char_index's cache. 0 = glyph 0; false = guest.
    bool charToGlyph(U32 font, U32 ch, U32& glyph) {
        if (ch > 0xffff) return gmWhy = "char over 0xffff", false; // get_char_index asks FreeType
        U32 data = m.rd(font + gmInfo[GI_FONT_PRIVATE]);
        if (!data) return gmWhy = "font not loaded", false;
        U32 face = m.rd(data + gmFt[GF_FT_FACE]);
        U32 charmap = face ? m.rd(face + gmFt[GF_CHARMAP]) : 0;
        if (!charmap) return gmWhy = "no charmap", false;
        U32 encoding = m.rd(charmap + gmFt[GF_ENCODING]);
        if (encoding == gmFt[GF_ENC_NONE] || encoding == gmFt[GF_ENC_SYMBOL]) return gmWhy = "symbol or no-encoding font", false;
        U32 block = m.rd(data + gmFt[GF_CHAR_INDEX] + (ch >> 8) * 4);
        U32 v = block ? m.rd(block + (ch & 0xff) * 4) : 0;
        if (!v) return gmWhy = "char not in char_index", false;
        glyph = v - 1;
        return m.ok;
    }
    // get_glyph_index_linked: font may become a child font
    bool charToGlyphLinked(U32& font, U32 ch, U32& glyph) {
        if (!charToGlyph(font, ch, glyph)) return false;
        if (glyph || ch < 32) return true;
        U32 head = font + gmInfo[GI_FONT_CHILDREN];
        U32 n = 0;
        for (U32 e = m.rd(head); e != head; e = m.rd(e)) {
            if (!m.ok || !e || ++n > 64) return gmWhy = "child font list", false;
            U32 child = e - gmInfo[GI_FONT_ENTRY], index;
            if (!m.rd(child + gmInfo[GI_FONT_PRIVATE])) return gmWhy = "child font not loaded", false;
            if (!charToGlyph(child, ch, index)) return false;
            if (index) {
                font = child;
                glyph = index;
                return true;
            }
        }
        return m.ok;
    }
    // get_gdi_font_glyph_metrics
    bool metrics(U32 font, U32 glyph, GmMetrics& out) {
        U32 block = glyph / gmInfo[GI_GM_BLOCK];
        if (block >= m.rd(font + gmInfo[GI_FONT_GM_SIZE])) return gmWhy = "metrics not cached", false;
        U32 blocks = m.rd(font + gmInfo[GI_FONT_GM]);
        U32 entries = blocks ? m.rd(blocks + block * 4) : 0;
        U32 entry = entries + (glyph % gmInfo[GI_GM_BLOCK]) * gmInfo[GI_GM_SIZE];
        if (!entries || !m.rd(entry + gmInfo[GI_GM_INIT])) return gmWhy = "metrics not cached", false;
        return m.read(entry + gmInfo[GI_GM_GM], out.gm, 20) && m.read(entry + gmInfo[GI_GM_ABC], &out.abcA, 12);
    }
    // The physdev's font when font.c would answer with font_lock free and the FreeType backend; 0 = guest.
    U32 font(U32 dev) {
        U32 font = m.rd(dev + gmInfo[GI_PHYSDEV_FONT]);
        if (!font) return gmWhy = "no font (next driver)", 0;
        if (m.rd(base + gmInfo[GI_FONT_LOCK])) return gmWhy = "font_lock held", 0; // glibc mutex __lock
        if (m.rd(base + gmInfo[GI_FONT_FUNCS]) != base + gmFt[GF_FUNCS]) return gmWhy = "other font backend", 0;
        return m.ok ? font : 0;
    }
    // get_glyph_outline's tategaki: get_gdi_font_name starts with '@' (either name, whichever it uses)
    bool vertical(U32 font) {
        U32 family = m.rd(font + gmInfo[GI_FONT_OTM_FAMILY]);
        if (!family || m.rd16(font + gmInfo[GI_FONT_LF_FACE]) == '@' || m.rd16(family) == '@') return gmWhy = "vertical font", true;
        return !m.ok;
    }
};

static bool gmGuestCall(U32 fn) {
    gmGuest[fn]++;
    if (gmGuest[fn] == 1) klog_fmt("[vnbox] native glyph metrics: a %s call runs as guest code (%s; after %u host calls)", gmNames[fn], gmWhy ? gmWhy : "arguments", gmCalls[fn]);
    gmWhy = nullptr;
    return false;
}

// Runs hooked function `fn` (hostGlyphmAt - 1) for the call the CPU is at (i386 cdecl), sets EAX and returns like
// its `ret`; false (nothing changed) leaves it to the guest function.
bool hostGlyphmRun(CPU* cpu, U32 fn) {
    if (fn >= GM_FNS || gmState != 1) return false;
    if (cpu->getDirection() < 0 || cpu->debugTrapActive || !cpu->isBig() || cpu->stackMask != 0xFFFFFFFF ||
        cpu->seg[CS].address || cpu->seg[DS].address || cpu->seg[ES].address || cpu->seg[SS].address) return gmGuestCall(fn);
    GmCall c;
    c.m.memory = cpu->memory;
    U32 a[7]; // the call's arguments: 7 for font_GetGlyphOutline, else 4
    if (!c.m.read(ESP + 4, a, fn == GM_GLYPH_OUTLINE ? 28 : 16)) return gmGuestCall(fn);
    c.base = cpu->seg[CS].address + cpu->eip.u32 - gmInfo[GI_EXTENT + fn];
    U32 font = c.font(a[0]);
    if (!font) return gmGuestCall(fn);
    U32 result = 1;
    GmMetrics gm;
    if (fn == GM_GLYPH_OUTLINE) { // (dev, glyph, format, gm, buflen, buf, mat)
        static const U8 identity[16] = { 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0 };
        U8 mat[16];
        U32 glyph = a[1], format = a[2];
        if (format != 0 && format != 0x80) return gmWhy = "format", gmGuestCall(fn); // GGO_METRICS (| GGO_GLYPH_INDEX)
        if (a[6] && (!c.m.read(a[6], mat, 16) || memcmp(mat, identity, 16))) return gmWhy = "matrix", gmGuestCall(fn);
        if (format == 0x80) {
            // font_funcs->get_glyph_index(font, &index, FALSE) leaves the index of a font with an encoding alone
            U32 data = c.m.rd(font + gmInfo[GI_FONT_PRIVATE]), face = data ? c.m.rd(data + gmFt[GF_FT_FACE]) : 0;
            U32 charmap = face ? c.m.rd(face + gmFt[GF_CHARMAP]) : 0;
            if (!charmap || c.m.rd(charmap + gmFt[GF_ENCODING]) == gmFt[GF_ENC_NONE]) return gmWhy = "no-encoding font", gmGuestCall(fn);
        } else if (c.vertical(font) || !c.charToGlyphLinked(font, glyph, glyph)) return gmGuestCall(fn);
        if (!c.metrics(font, glyph, gm) || !c.m.ok || (a[3] && !c.m.range(a[3], 20, true))) return gmGuestCall(fn);
        if (a[3]) c.m.copy(a[3], gm.gm, 20, true);
    } else { // (dev, str or indices, count, dxs)
        S32 count = (S32)a[2];
        if (count <= 0 || count > 0x100000 || (fn == GM_EXTENT && c.vertical(font))) return gmGuestCall(fn);
        static thread_local std::vector<U16> chars;
        static thread_local std::vector<S32> dxs;
        chars.resize(count);
        dxs.resize(count);
        if (!c.m.read(a[1], chars.data(), count * 2)) return gmGuestCall(fn);
        if (fn == GM_EXTENT_I) {
            U32 data = c.m.rd(font + gmInfo[GI_FONT_PRIVATE]), face = data ? c.m.rd(data + gmFt[GF_FT_FACE]) : 0;
            U32 charmap = face ? c.m.rd(face + gmFt[GF_CHARMAP]) : 0;
            if (!charmap || c.m.rd(charmap + gmFt[GF_ENCODING]) == gmFt[GF_ENC_NONE]) return gmWhy = "no-encoding font", gmGuestCall(fn);
        }
        U32 pos = 0; // pos += abc.abcA + abc.abcB + abc.abcC, in C's unsigned arithmetic
        for (S32 i = 0; i < count; i++) {
            U32 f = font, glyph = chars[i];
            if (fn == GM_EXTENT && !c.charToGlyphLinked(f, glyph, glyph)) return gmGuestCall(fn);
            if (!c.metrics(f, glyph, gm)) return gmGuestCall(fn);
            pos += (U32)gm.abcA + gm.abcB + (U32)gm.abcC;
            dxs[i] = (S32)pos;
        }
        if (!c.m.ok || !c.m.range(a[3], count * 4, true)) return gmGuestCall(fn);
        c.m.copy(a[3], (U8*)dxs.data(), count * 4, true);
    }
    if (++gmCalls[fn] == 1 || gmCalls[fn] == 1000 || gmCalls[fn] == 1000000) klog_fmt("[vnbox] native glyph metrics: %u %s calls as host code (%u as guest code)", gmCalls[fn], gmNames[fn], gmGuest[fn]);
    EAX = result;
    cpu->eip.u32 = cpu->pop32();
    return true;
}
#else
bool hostGlyphmMapped(KFile*, std::vector<U32>&) { return false; }
U32 hostGlyphmAt(const BString&, U32) { return 0; }
bool hostGlyphmRun(CPU*, U32) { return false; }
#endif
