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

// Host FreeType: Wine's win32u.so loads, hints (TT_RunIns, the TrueType bytecode interpreter) and rasterizes every
// GDI glyph through the root fs's libfreetype.so.6, emulated. Text games that redraw their text every frame spend a
// large share of the guest's time there. The exports win32u calls on its fonts get a head op (hostLibcOp, the native
// libc mechanism in hosthooks.cpp) and run on host FreeType (lib/freetype: the same version and options, so the same
// bytes come out) instead.
//
// A face is host or guest for its whole life: the faces win32u opens with FT_New_Memory_Face (every font it draws
// with) are opened on the host, on a copy of the font bytes, in a host twin of the guest FT_Library. Every other face
// (win32u's FT_New_Face while it lists the installed fonts, fontconfig's, dwrite's) stays guest code, and so does
// every call on it: a hooked function that gets a face, slot or library it doesn't know runs the guest function.
// The guest reads a host face's public fields directly (face->glyph->outline, ->size->metrics, ->charmaps, the
// sfnt tables...), so each host face has a mirror in guest memory (mmap'ed by the host, in the process): the public
// structs with guest pointers, kept in step after every call that changes them. The guest may change the glyph's
// outline points (FT_Outline_Transform/Translate/Embolden run as guest code on the mirror); FT_Render_Glyph takes
// them back first. FT_Outline_Get_Bitmap is a pure function of its arguments and runs on the host for any caller.
// FT_Library_SetLcdFilter and FT_Property_Set run on the twin too, then as guest code.
// nativeFreetype=0 (URL) turns it off.
#include "../../softmmu/kmemory_soft.h"
#include <map>
#include <tuple>
#ifdef BOXEDWINE_HOST_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_LCD_FILTER_H
#include FT_TRUETYPE_TABLES_H
#include FT_SFNT_NAMES_H
#include FT_WINFONTS_H
#include FT_MODULE_H
#endif

enum : U32 {
    FTH_NEW_MEMORY_FACE, FTH_DONE_FACE, FTH_SET_PIXEL_SIZES, FTH_SET_CHARMAP, FTH_GET_CHAR_INDEX, FTH_GET_FIRST_CHAR,
    FTH_GET_NEXT_CHAR, FTH_GET_SFNT_NAME_COUNT, FTH_GET_SFNT_NAME, FTH_GET_SFNT_TABLE, FTH_LOAD_SFNT_TABLE,
    FTH_GET_WINFNT_HEADER, FTH_LOAD_GLYPH, FTH_RENDER_GLYPH, FTH_OUTLINE_GET_BITMAP, // host for host objects
    FTH_LIBRARY_SET_LCD_FILTER, FTH_PROPERTY_SET, // mirrored on the twin library, then guest
    FTH_COUNT
};
static const char* const hostFreetypeNames[FTH_COUNT] = {
    "FT_New_Memory_Face", "FT_Done_Face", "FT_Set_Pixel_Sizes", "FT_Set_Charmap", "FT_Get_Char_Index", "FT_Get_First_Char",
    "FT_Get_Next_Char", "FT_Get_Sfnt_Name_Count", "FT_Get_Sfnt_Name", "FT_Get_Sfnt_Table", "FT_Load_Sfnt_Table",
    "FT_Get_WinFNT_Header", "FT_Load_Glyph", "FT_Render_Glyph", "FT_Outline_Get_Bitmap",
    "FT_Library_SetLcdFilter", "FT_Property_Set"
};

U32 hostFreetypeFnId(const char* name) { // FTH_* + 1, or 0
    for (U32 i = 0; i < FTH_COUNT; i++) {
        if (!strcmp(hostFreetypeNames[i], name)) return i + 1;
    }
    return 0;
}

// Whether libfreetype.so.6's exports get hooked at all (read once, when it is first mapped).
bool hostFreetypeEnabled() {
#ifdef BOXEDWINE_HOST_FREETYPE
#ifdef __EMSCRIPTEN__
    if (vnboxUrlIs("nativeFreetype", "0")) {
        klog_fmt("[vnbox] native freetype off (nativeFreetype=0)");
        return false;
    }
#endif
    return true;
#else
    return false;
#endif
}

#ifdef BOXEDWINE_HOST_FREETYPE
// The guest's libfreetype.so.6 is 32-bit x86 (ILP32, like wasm32): the public structs have the same layout on both
// sides, pointers included, so a host struct is copied as is and its pointers replaced by guest ones.
// FreeType 2.11.1 sizes with gcc -m32 (i386 SysV).
static_assert(sizeof(void*) == 4 && sizeof(long) == 4, "host FreeType mirrors need a 32-bit host");
static_assert(sizeof(FT_FaceRec) == 132 && offsetof(FT_FaceRec, glyph) == 84 && offsetof(FT_FaceRec, autohint) == 116, "FT_FaceRec layout");
static_assert(sizeof(FT_CharMapRec) == 12 && sizeof(FT_Bitmap_Size) == 16 && sizeof(FT_SizeRec) == 44 && offsetof(FT_SizeRec, metrics) == 12, "FT_SizeRec layout");
static_assert(sizeof(FT_GlyphSlotRec) == 160 && offsetof(FT_GlyphSlotRec, bitmap) == 76 && offsetof(FT_GlyphSlotRec, outline) == 108, "FT_GlyphSlotRec layout");
static_assert(sizeof(FT_Bitmap) == 24 && sizeof(FT_Outline) == 20 && sizeof(FT_SfntName) == 16 && sizeof(FT_WinFNT_HeaderRec) == 156, "struct layout");
static_assert(sizeof(TT_Header) == 56 && sizeof(TT_MaxProfile) == 32 && sizeof(TT_OS2) == 104 && sizeof(TT_HoriHeader) == 44 &&
    sizeof(TT_VertHeader) == 44 && sizeof(TT_Postscript) == 32 && sizeof(TT_PCLT) == 56 && offsetof(TT_HoriHeader, long_metrics) == 36, "sfnt table layout");
static const U32 sfntTableSizes[FT_SFNT_MAX] = { sizeof(TT_Header), sizeof(TT_MaxProfile), sizeof(TT_OS2), sizeof(TT_HoriHeader),
    sizeof(TT_VertHeader), sizeof(TT_Postscript), sizeof(TT_PCLT) };

template <typename T> static T* gptr(U32 address) { return (T*)(uintptr_t)address; } // a guest address in a host struct

struct HostFtLibrary {
    FT_Library lib = nullptr;
    bool ok = true; // false: a setting the twin can't follow was made, new faces stay guest
};
struct HostFtData { std::vector<U8> bytes; U32 refs = 0; };
struct HostFtFace {
    KProcess* process;
    U32 pid, guestLib;
    FT_Face face = nullptr;
    std::shared_ptr<HostFtData> data;
    U64 dataKey;
    std::vector<std::pair<U32, U32>> chunks; // guest mmaps: address, length
    U32 next = 0, end = 0; // bump allocation in the last chunk
    U32 gFace = 0, gCharmaps = 0, gCharmapPtrs = 0, gSizes = 0, gSize = 0, gSlot = 0, gFamily = 0, gStyle = 0;
    U32 gTables[FT_SFNT_MAX] = {};
    U32 gPoints = 0, pointsCap = 0, gTags = 0, tagsCap = 0, gContours = 0, contoursCap = 0, gBitmap = 0, bitmapCap = 0;
    std::unordered_map<U32, U32> names; // sfnt name index -> guest copy of its string
};
static std::unordered_map<U64, HostFtLibrary> hostFtLibraries; // pid << 32 | guest FT_Library
static std::unordered_map<U64, std::unique_ptr<HostFtFace>> hostFtFaces; // pid << 32 | guest face mirror
static std::unordered_map<U64, HostFtFace*> hostFtSlots; // pid << 32 | guest glyph slot mirror
static std::map<std::tuple<U32, U32, U32>, std::weak_ptr<HostFtData>> hostFtDatas; // pid, guest base, size

static void hostFtFree(U64 key, bool unmap) {
    auto it = hostFtFaces.find(key);
    if (it == hostFtFaces.end()) return;
    HostFtFace* f = it->second.get();
    if (f->face) FT_Done_Face(f->face);
    if (unmap) {
        for (auto& c : f->chunks) f->process->memory->unmap(c.first, c.second);
    }
    hostFtSlots.erase(((U64)f->pid << 32) | f->gSlot);
    hostFtFaces.erase(it);
}

// Faces of processes that are gone (the pid may be in use again by another process): host side only.
static void hostFtPurge() {
    std::vector<U64> stale;
    for (auto& e : hostFtFaces) {
        KProcessPtr p = KSystem::getProcess(e.second->pid);
        if (!p || p.get() != e.second->process) stale.push_back(e.first);
    }
    for (U64 key : stale) hostFtFree(key, false);
    for (auto it = hostFtDatas.begin(); it != hostFtDatas.end();) {
        if (it->second.expired()) it = hostFtDatas.erase(it);
        else ++it;
    }
}

static HostFtFace* hostFtFace(CPU* cpu, U32 face) {
    auto it = hostFtFaces.find(((U64)cpu->thread->process->id << 32) | face);
    if (it == hostFtFaces.end() || !face) return nullptr;
    if (it->second->process != cpu->thread->process.get()) { // a pid used again
        hostFtFree(it->first, false);
        return nullptr;
    }
    return it->second.get();
}

static HostFtLibrary* hostFtLibrary(U32 pid, U32 guestLib) {
    if (!guestLib) return nullptr;
    HostFtLibrary& l = hostFtLibraries[((U64)pid << 32) | guestLib];
    if (!l.lib && l.ok && FT_Init_FreeType(&l.lib)) {
        l.lib = nullptr;
        l.ok = false;
    }
    return l.ok ? &l : nullptr;
}

// Guest memory for a face's mirror, from guest mmaps of at least 64K owned by the face. 0 when the guest is out of
// address space.
static U32 hostFtAlloc(CPU* cpu, HostFtFace& f, U32 len) {
    len = (len + 15) & ~15u;
    if (f.end - f.next < len || !f.next) {
        U32 size = std::max(0x10000u, (len + K_PAGE_SIZE - 1) & ~(K_PAGE_SIZE - 1));
        U32 a = cpu->memory->mmap(cpu->thread, 0, size, K_PROT_READ | K_PROT_WRITE, K_MAP_PRIVATE | K_MAP_ANONYMOUS, -1, 0);
        if (!a || a >= 0xFFFFF000) return 0;
        f.chunks.push_back({ a, size });
        f.next = a;
        f.end = a + size;
    }
    U32 a = f.next;
    f.next += len;
    return a;
}

// Guest buffer of at least len bytes in *buf (grown by doubling), 0 on failure.
static U32 hostFtBuffer(CPU* cpu, HostFtFace& f, U32& buf, U32& cap, U32 len) {
    if (len > cap) {
        U32 want = std::max(len, cap * 2);
        U32 a = hostFtAlloc(cpu, f, want);
        if (!a) return 0;
        buf = a;
        cap = want;
    }
    return buf ? buf : hostFtAlloc(cpu, f, 16);
}

static U32 hostFtString(CPU* cpu, HostFtFace& f, const char* s) {
    if (!s) return 0;
    U32 len = (U32)strlen(s) + 1, a = hostFtAlloc(cpu, f, len);
    if (a) cpu->memory->memcpy(a, s, len);
    return a;
}

static U32 hostFtCharmap(const HostFtFace& f, FT_CharMap cm) {
    for (int i = 0; i < f.face->num_charmaps; i++) {
        if (f.face->charmaps[i] == cm) return f.gCharmaps + i * sizeof(FT_CharMapRec);
    }
    return 0;
}

// The face record and its size record, as the host's now (charmap and size metrics change).
static void hostFtSyncFace(CPU* cpu, HostFtFace& f) {
    FT_FaceRec r = *f.face;
    r.family_name = gptr<FT_String>(f.gFamily);
    r.style_name = gptr<FT_String>(f.gStyle);
    r.available_sizes = gptr<FT_Bitmap_Size>(r.num_fixed_sizes ? f.gSizes : 0);
    r.charmaps = gptr<FT_CharMap>(r.num_charmaps ? f.gCharmapPtrs : 0);
    r.generic = {};
    r.glyph = gptr<FT_GlyphSlotRec>(f.gSlot);
    r.size = gptr<FT_SizeRec>(f.gSize);
    r.charmap = gptr<FT_CharMapRec>(hostFtCharmap(f, f.face->charmap));
    r.driver = nullptr;
    r.memory = nullptr;
    r.stream = nullptr;
    r.sizes_list = {};
    r.autohint = {};
    r.extensions = nullptr;
    r.internal = nullptr;
    cpu->memory->memcpy(f.gFace, &r, sizeof(r));
    FT_SizeRec s = {};
    if (f.face->size) s = *f.face->size;
    s.face = gptr<FT_FaceRec>(f.gFace);
    s.generic = {};
    s.internal = nullptr;
    cpu->memory->memcpy(f.gSize, &s, sizeof(s));
}

// The glyph slot (metrics, outline, bitmap), as the host's now. False when the guest is out of memory for it.
static bool hostFtSyncSlot(CPU* cpu, HostFtFace& f) {
    FT_GlyphSlot slot = f.face->glyph;
    FT_GlyphSlotRec r = *slot;
    const FT_Outline& o = slot->outline;
    U32 points = o.n_points > 0 ? (U32)o.n_points : 0, contours = o.n_contours > 0 ? (U32)o.n_contours : 0;
    U32 gp = hostFtBuffer(cpu, f, f.gPoints, f.pointsCap, points * sizeof(FT_Vector));
    U32 gt = hostFtBuffer(cpu, f, f.gTags, f.tagsCap, points);
    U32 gc = hostFtBuffer(cpu, f, f.gContours, f.contoursCap, contours * sizeof(short));
    U32 bitmapLen = slot->bitmap.buffer ? slot->bitmap.rows * (U32)std::abs(slot->bitmap.pitch) : 0;
    U32 gb = hostFtBuffer(cpu, f, f.gBitmap, f.bitmapCap, bitmapLen);
    if (!gp || !gt || !gc || !gb) return false;
    if (points) {
        cpu->memory->memcpy(gp, o.points, points * sizeof(FT_Vector));
        cpu->memory->memcpy(gt, o.tags, points);
    }
    if (contours) cpu->memory->memcpy(gc, o.contours, contours * sizeof(short));
    if (bitmapLen) cpu->memory->memcpy(gb, slot->bitmap.buffer, bitmapLen);
    r.library = gptr<FT_LibraryRec_>(f.guestLib);
    r.face = gptr<FT_FaceRec>(f.gFace);
    r.next = nullptr;
    r.generic = {};
    r.bitmap.buffer = gptr<unsigned char>(slot->bitmap.buffer ? gb : 0);
    r.bitmap.palette = nullptr;
    r.outline.points = gptr<FT_Vector>(o.points ? gp : 0);
    r.outline.tags = gptr<char>(o.tags ? gt : 0);
    r.outline.contours = gptr<short>(o.contours ? gc : 0);
    r.subglyphs = nullptr; // shortcut: FT_LOAD_NO_RECURSE's subglyphs aren't mirrored (win32u never asks for them)
    r.control_data = nullptr;
    r.control_len = 0;
    r.other = nullptr;
    r.internal = nullptr;
    cpu->memory->memcpy(f.gSlot, &r, sizeof(r));
    return true;
}

// The mirror of a new host face: the records, then the arrays and strings they point to.
static bool hostFtMirror(CPU* cpu, HostFtFace& f) {
    FT_Face face = f.face;
    U32 nc = face->num_charmaps > 0 ? (U32)face->num_charmaps : 0, ns = face->num_fixed_sizes > 0 ? (U32)face->num_fixed_sizes : 0;
    f.gFace = hostFtAlloc(cpu, f, sizeof(FT_FaceRec));
    f.gSize = hostFtAlloc(cpu, f, sizeof(FT_SizeRec));
    f.gSlot = hostFtAlloc(cpu, f, sizeof(FT_GlyphSlotRec));
    f.gCharmaps = hostFtAlloc(cpu, f, nc * sizeof(FT_CharMapRec) + 4);
    f.gCharmapPtrs = hostFtAlloc(cpu, f, nc * 4 + 4);
    f.gSizes = hostFtAlloc(cpu, f, ns * sizeof(FT_Bitmap_Size) + 4);
    if (!f.gFace || !f.gSize || !f.gSlot || !f.gCharmaps || !f.gCharmapPtrs || !f.gSizes) return false;
    if (face->family_name && !(f.gFamily = hostFtString(cpu, f, face->family_name))) return false;
    if (face->style_name && !(f.gStyle = hostFtString(cpu, f, face->style_name))) return false;
    for (U32 i = 0; i < nc; i++) {
        FT_CharMapRec c = *face->charmaps[i];
        c.face = gptr<FT_FaceRec>(f.gFace);
        U32 a = f.gCharmaps + i * sizeof(FT_CharMapRec);
        cpu->memory->memcpy(a, &c, sizeof(c));
        cpu->memory->writed(f.gCharmapPtrs + i * 4, a);
    }
    if (ns) cpu->memory->memcpy(f.gSizes, face->available_sizes, ns * sizeof(FT_Bitmap_Size));
    for (U32 tag = 0; tag < FT_SFNT_MAX; tag++) { // the tables don't change after loading
        void* t = FT_Get_Sfnt_Table(face, (FT_Sfnt_Tag)tag);
        if (!t) continue;
        if (!(f.gTables[tag] = hostFtAlloc(cpu, f, sfntTableSizes[tag]))) return false;
        U8 copy[sizeof(TT_OS2)];
        ::memcpy(copy, t, sfntTableSizes[tag]);
        if (tag == FT_SFNT_HHEA || tag == FT_SFNT_VHEA) { // long_metrics, short_metrics: FreeType's own
            ::memset(copy + offsetof(TT_HoriHeader, long_metrics), 0, 8);
        }
        cpu->memory->memcpy(f.gTables[tag], copy, sfntTableSizes[tag]);
    }
    hostFtSyncFace(cpu, f);
    return hostFtSyncSlot(cpu, f);
}

#ifdef __TEST
U32 hostFreetypeTestCaller; // testHostFreetype.cpp: a return address that counts as in win32u.so
#endif

// FT_New_Memory_Face(library, base, size, face_index, aface) from win32u: a host face, or false (guest) when the
// guest can't be followed.
static bool hostFtNewMemoryFace(CPU* cpu, U32 lib, U32 base, U32 size, S32 index, U32 aface, U32 returnAddress, S32& result) {
    KThread* thread = cpu->thread;
    KProcess* process = thread->process.get();
    KMemory* memory = cpu->memory;
    bool fromWin32u = process->getModuleName(returnAddress).endsWith("/win32u.so"); // its FT_* calls are all hooked
#ifdef __TEST
    fromWin32u = fromWin32u || (hostFreetypeTestCaller && returnAddress == hostFreetypeTestCaller);
#endif
    if (!fromWin32u) return false;
    HostFtLibrary* l = hostFtLibrary(process->id, lib);
    if (!l || !aface || !memory->canWrite(aface, 4) || !size || size > 0x10000000 || !memory->canRead(base, size)) return false;
    hostFtPurge();
    auto key = std::make_tuple(process->id, base, size);
    std::shared_ptr<HostFtData> data = hostFtDatas[key].lock(); // shared with live faces of the same guest mapping only
    if (!data) {
        data = std::make_shared<HostFtData>();
        data->bytes.resize(size);
        memory->memcpy(data->bytes.data(), base, size);
        hostFtDatas[key] = data;
    }
    auto f = std::make_unique<HostFtFace>();
    f->process = process;
    f->pid = process->id;
    f->guestLib = lib;
    f->data = data;
    FT_Error err = FT_New_Memory_Face(l->lib, data->bytes.data(), (FT_Long)size, index, &f->face);
    if (err) { // the same error the guest gets (same FreeType)
        memory->writed(aface, 0);
        result = err;
        return true;
    }
    if (!hostFtMirror(cpu, *f)) {
        klog_fmt("[vnbox] native freetype: no guest memory for a face mirror, face left to the guest");
        for (auto& c : f->chunks) memory->unmap(c.first, c.second);
        FT_Done_Face(f->face);
        return false;
    }
    static bool logged;
    if (!logged) klog_fmt("[vnbox] native freetype: first host face (%s %s) in pid %d", f->face->family_name ? f->face->family_name : "?", f->face->style_name ? f->face->style_name : "", f->pid);
    logged = true;
    U64 pid = (U64)f->pid << 32;
    memory->writed(aface, f->gFace);
    hostFtSlots[pid | f->gSlot] = f.get();
    hostFtFaces[pid | f->gFace] = std::move(f);
    result = 0;
    return true;
}

// FT_Outline_Get_Bitmap(library, outline, bitmap) on host copies of the guest's outline arrays and bitmap buffer.
static bool hostFtOutlineGetBitmap(CPU* cpu, U32 lib, U32 outline, U32 bitmap, S32& result) {
    KMemory* memory = cpu->memory;
    HostFtLibrary* l = hostFtLibrary(cpu->thread->process->id, lib);
    if (!l || !memory->canRead(outline, sizeof(FT_Outline)) || !memory->canRead(bitmap, sizeof(FT_Bitmap))) return false;
    FT_Outline o;
    FT_Bitmap b;
    memory->memcpy(&o, outline, sizeof(o));
    memory->memcpy(&b, bitmap, sizeof(b));
    U32 gp = (U32)(uintptr_t)o.points, gt = (U32)(uintptr_t)o.tags, gc = (U32)(uintptr_t)o.contours, gb = (U32)(uintptr_t)b.buffer;
    if (o.n_points < 0 || o.n_contours < 0 || !gb || (o.n_points && (!gp || !gt)) || (o.n_contours && !gc)) return false; // the guest's errors
    U32 points = (U32)o.n_points, contours = (U32)o.n_contours;
    U64 len = (U64)b.rows * (U32)std::abs(b.pitch);
    if (len > 0x4000000 || (points && (!memory->canRead(gp, points * sizeof(FT_Vector)) || !memory->canRead(gt, points))) ||
        (contours && !memory->canRead(gc, contours * sizeof(short))) || (len && !memory->canWrite(gb, (U32)len))) return false;
    static thread_local std::vector<U8> arrays, pixels;
    arrays.resize(points * (sizeof(FT_Vector) + 1) + contours * sizeof(short) + 8);
    pixels.resize(len ? (size_t)len : 1);
    U8* p = arrays.data();
    if (points) memory->memcpy(p, gp, points * sizeof(FT_Vector));
    if (points) memory->memcpy(p + points * sizeof(FT_Vector), gt, points);
    if (contours) memory->memcpy(p + points * (sizeof(FT_Vector) + 1) + (points & 1), gc, contours * sizeof(short));
    o.points = (FT_Vector*)p;
    o.tags = (char*)(p + points * sizeof(FT_Vector));
    o.contours = (short*)(p + points * (sizeof(FT_Vector) + 1) + (points & 1));
    if (len) memory->memcpy(pixels.data(), gb, (U32)len); // the rasterizers draw into what is there
    b.buffer = pixels.data();
    b.palette = nullptr;
    result = FT_Outline_Get_Bitmap(l->lib, &o, &b);
    if (len) memory->memcpy(gb, pixels.data(), (U32)len);
    return true;
}

// FT_Render_Glyph(slot, mode) on a host slot: the guest may have moved the outline's points (transform, translate,
// embolden as guest code), so they come back first.
static bool hostFtRenderGlyph(CPU* cpu, HostFtFace& f, U32 mode, S32& result) {
    KMemory* memory = cpu->memory;
    FT_GlyphSlot slot = f.face->glyph;
    FT_Outline o;
    memory->memcpy(&o, f.gSlot + offsetof(FT_GlyphSlotRec, outline), sizeof(o));
    if (slot->format == FT_GLYPH_FORMAT_OUTLINE) {
        U32 gp = (U32)(uintptr_t)o.points;
        if (o.n_points != slot->outline.n_points || o.n_contours != slot->outline.n_contours ||
            (o.n_points > 0 && !memory->canRead(gp, (U32)o.n_points * sizeof(FT_Vector)))) {
            static bool logged;
            if (!logged) klog_fmt("[vnbox] native freetype: FT_Render_Glyph on an outline the guest reshaped, not rendered");
            logged = true;
            result = FT_Err_Invalid_Outline;
            return true;
        }
        if (o.n_points > 0) memory->memcpy(slot->outline.points, gp, (U32)o.n_points * sizeof(FT_Vector));
        slot->outline.flags = o.flags;
    }
    result = FT_Render_Glyph(slot, (FT_Render_Mode)mode);
    if (!hostFtSyncSlot(cpu, f)) result = FT_Err_Out_Of_Memory;
    return true;
}
#endif

// Runs the hooked FreeType export fn (FTH_*) for the call the CPU is at (cdecl). True when the call was done here
// (EAX set, returned); false runs the guest function, after any mirrored side effect.
bool hostFreetypeRun(CPU* cpu, U32 fn) {
#ifdef BOXEDWINE_HOST_FREETYPE
    KMemory* memory = cpu->memory;
    // A debug trap only sends what guest code can do to the guest (a new face, FT_Outline_Get_Bitmap). A host face stays on
    // the host: its mirror has no driver, so guest code would fault on it. shortcut: its writes don't fire a watchpoint.
    if (!cpu->isBig() || cpu->stackMask != 0xFFFFFFFF || cpu->seg[CS].address || cpu->seg[DS].address ||
        cpu->seg[ES].address || cpu->seg[SS].address || !memory->canRead(ESP, 24)) return false;
    U32 ret = memory->readd(ESP), a0 = memory->readd(ESP + 4), a1 = memory->readd(ESP + 8), a2 = memory->readd(ESP + 12);
    U32 a3 = memory->readd(ESP + 16), a4 = memory->readd(ESP + 20);
    S32 result = 0;
    switch (fn) {
    case FTH_NEW_MEMORY_FACE:
        if (cpu->debugTrapActive || !hostFtNewMemoryFace(cpu, a0, a1, a2, (S32)a3, a4, ret, result)) return false;
        break;
    case FTH_OUTLINE_GET_BITMAP:
        if (cpu->debugTrapActive || !hostFtOutlineGetBitmap(cpu, a0, a1, a2, result)) return false;
        break;
    case FTH_LIBRARY_SET_LCD_FILTER:
        if (HostFtLibrary* l = hostFtLibrary(cpu->thread->process->id, a0)) FT_Library_SetLcdFilter(l->lib, (FT_LcdFilter)a1);
        return false;
    case FTH_PROPERTY_SET: { // (library, module_name, property_name, value): FT_UInt properties are followed
        HostFtLibrary* l = hostFtLibrary(cpu->thread->process->id, a0);
        if (!l) return false;
        BString module = memory->readString(a1), property = memory->readString(a2);
        if ((property == "interpreter-version" || property == "hinting-engine") && memory->canRead(a3, 4)) {
            FT_UInt value = memory->readd(a3);
            FT_Property_Set(l->lib, module.c_str(), property.c_str(), &value);
        } else {
            klog_fmt("[vnbox] native freetype: property %s.%s set, this library's new faces stay guest code", module.c_str(), property.c_str());
            l->ok = false;
        }
        return false;
    }
    case FTH_RENDER_GLYPH: {
        auto it = hostFtSlots.find(((U64)cpu->thread->process->id << 32) | a0);
        if (it == hostFtSlots.end() || !a0 || !hostFtFace(cpu, it->second->gFace)) return false;
        hostFtRenderGlyph(cpu, *it->second, a1, result);
        break;
    }
    default: { // the face calls
        HostFtFace* f = hostFtFace(cpu, a0);
        if (!f) return false;
        FT_Face face = f->face;
        switch (fn) {
        case FTH_DONE_FACE:
            hostFtFree(((U64)f->pid << 32) | f->gFace, true);
            break;
        case FTH_SET_PIXEL_SIZES:
            result = FT_Set_Pixel_Sizes(face, a1, a2);
            hostFtSyncFace(cpu, *f);
            break;
        case FTH_SET_CHARMAP: {
            FT_CharMap cm = nullptr;
            for (int i = 0; i < face->num_charmaps; i++) {
                if (a1 == f->gCharmaps + i * sizeof(FT_CharMapRec)) cm = face->charmaps[i];
            }
            // FreeType compares the pointer with the face's charmaps: one it doesn't have is Invalid_Argument
            result = cm ? FT_Set_Charmap(face, cm) : !face->charmaps || !a1 ? FT_Err_Invalid_CharMap_Handle : FT_Err_Invalid_Argument;
            hostFtSyncFace(cpu, *f);
            break;
        }
        case FTH_GET_CHAR_INDEX:
            result = (S32)FT_Get_Char_Index(face, a1);
            break;
        case FTH_GET_FIRST_CHAR:
        case FTH_GET_NEXT_CHAR: {
            U32 agindex = fn == FTH_GET_FIRST_CHAR ? a1 : a2;
            if (agindex && !memory->canWrite(agindex, 4)) { result = 0; break; } // shortcut: where the guest would fault
            FT_UInt gindex = 0;
            result = (S32)(fn == FTH_GET_FIRST_CHAR ? FT_Get_First_Char(face, &gindex) : FT_Get_Next_Char(face, a1, &gindex));
            if (agindex) memory->writed(agindex, gindex);
            break;
        }
        case FTH_GET_SFNT_NAME_COUNT:
            result = (S32)FT_Get_Sfnt_Name_Count(face);
            break;
        case FTH_GET_SFNT_NAME: { // (face, idx, aname)
            FT_SfntName name;
            if (!memory->canWrite(a2, sizeof(name))) { result = FT_Err_Invalid_Argument; break; }
            result = FT_Get_Sfnt_Name(face, a1, &name);
            if (result) break;
            U32& copy = f->names[a1];
            if (!copy && name.string_len && !(copy = hostFtAlloc(cpu, *f, name.string_len))) { result = FT_Err_Out_Of_Memory; break; }
            if (name.string_len) memory->memcpy(copy, name.string, name.string_len);
            name.string = gptr<FT_Byte>(name.string_len ? copy : 0);
            memory->memcpy(a2, &name, sizeof(name));
            break;
        }
        case FTH_GET_SFNT_TABLE:
            result = a1 < FT_SFNT_MAX ? (S32)f->gTables[a1] : 0;
            break;
        case FTH_LOAD_SFNT_TABLE: { // (face, tag, offset, buffer, length): *length 0 asks for the size, NULL reads the whole table
            // shortcut: unwritable arguments and a read into a NULL buffer, where the guest would fault, give Invalid_Argument
            if (a4 && !memory->canWrite(a4, 4)) { result = FT_Err_Invalid_Argument; break; }
            FT_ULong length = a4 ? memory->readd(a4) : 0;
            if (a4 && !length) {
                result = FT_Load_Sfnt_Table(face, a1, (FT_Long)a2, nullptr, &length);
                if (!result) memory->writed(a4, (U32)length);
                break;
            }
            if (!a4 && (result = FT_Load_Sfnt_Table(face, a1, 0, nullptr, &length))) break;
            if (!a3 || length > 0x10000000 || !memory->canWrite(a3, (U32)length)) { result = FT_Err_Invalid_Argument; break; }
            static thread_local std::vector<U8> buf;
            buf.resize(length + 1);
            result = FT_Load_Sfnt_Table(face, a1, (FT_Long)a2, buf.data(), a4 ? &length : nullptr);
            if (!result) memory->memcpy(a3, buf.data(), (U32)length);
            break;
        }
        case FTH_GET_WINFNT_HEADER: {
            FT_WinFNT_HeaderRec header;
            if (!memory->canWrite(a1, sizeof(header))) { result = FT_Err_Invalid_Argument; break; }
            result = FT_Get_WinFNT_Header(face, &header);
            if (!result) memory->memcpy(a1, &header, sizeof(header));
            break;
        }
        case FTH_LOAD_GLYPH:
            result = FT_Load_Glyph(face, a1, (FT_Int32)a2);
            if (!hostFtSyncSlot(cpu, *f)) result = FT_Err_Out_Of_Memory;
            break;
        default:
            return false;
        }
    }
    }
    EAX = (U32)result;
    cpu->eip.u32 = cpu->pop32();
    return true;
#else
    return false;
#endif
}
