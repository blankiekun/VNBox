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

// Native libvorbis: games that ship libvorbis.dll (イマ, Muramasa, ...) decode their Ogg audio through it, emulated, in
// x87 code the JIT runs ~40x slower than native. The decode-side exports get a head op (hostLibcOp, the native libc
// mechanism in hosthooks.cpp) and run on host libvorbis (Emscripten port) instead, on a host copy of each guest
// object: vorbis_info by the guest vi's codec_setup pointer (vorbisfile copies vorbis_info structs by value, so the
// vi address a stream is decoded with isn't always the one its headers went into), vorbis_dsp_state by guest vd,
// vorbis_block by guest vb.
//
// - Host only (the guest function never runs): vorbis_synthesis, _trackonly, _blockin, _pcmout, _read. After each,
//   the guest vd's public fields are set from the host's; _pcmout copies the host's pending samples into the guest's
//   own pcm buffers (allocated by the guest's vorbis_synthesis_init, same pcm_storage) at the same index.
// - Mirrored (host side effect, then the guest function runs as usual, so the guest objects stay what callers that
//   read them, like vorbisfile, expect): _headerin (an id header restarts the host vi), _synthesis_init, block_init,
//   _restart, _halfrate, and the clears (free the host copy). libvorbis calls some of these itself (init → restart,
//   a bad header → info_clear), which mirrors the same way.
// - A dsp state is checked at its first host-only call, before anything diverges: the guest vd must point at the
//   guest vi it was initialized with and have the host's pcm_storage and channels (public ABI of libvorbis 1.x,
//   MSVC/MinGW 32-bit layout). Otherwise it, and anything without a host copy, stays guest code for good.
//
// libvorbis.dll isn't file-mapped (Wine reads a PE whose file alignment isn't the page size into anonymous memory),
// so hostLibcMapped never sees it. Instead the first block decoded in each 64K region of a process looks down for a
// PE image (they are 64K aligned) and reads its exports from guest memory; an image exporting the needed vorbis_*
// functions gets them hooked. A hooked address must still start with the bytes seen then.
// libvorbisfile's ov_read 16-bit pack loop is hosted too, for one known build (hostVorbisFileParse).
// nativeVorbis=0 (URL) turns it all off, nativeVorbisfile=0 only the ov_read loop. Host libvorbis 1.3.7 vs the game's build may round samples differently.
#include "../../softmmu/kmemory_soft.h"
#include "../../softmmu/soft_mmu.h"
#include "../../softmmu/soft_ram.h"
#ifdef BOXEDWINE_HOST_VORBIS
#include <ogg/ogg.h>
#include <vorbis/codec.h>
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

enum : U32 {
    VB_SYNTHESIS, VB_TRACKONLY, VB_BLOCKIN, VB_PCMOUT, VB_READ, // host only
    VB_HEADERIN, VB_SYNTHESIS_INIT, VB_BLOCK_INIT, VB_RESTART, VB_HALFRATE, VB_BLOCK_CLEAR, VB_DSP_CLEAR, VB_INFO_CLEAR, // mirrored
    VB_LAPOUT, // only via ov_crosslap: logged, guest
    VB_PACK16, // not an export: libvorbisfile's ov_read 16-bit pack loop (hostVorbisFileParse)
    VB_COUNT
};
static const char* const hostVorbisNames[VB_COUNT] = {
    "vorbis_synthesis", "vorbis_synthesis_trackonly", "vorbis_synthesis_blockin", "vorbis_synthesis_pcmout",
    "vorbis_synthesis_read", "vorbis_synthesis_headerin", "vorbis_synthesis_init", "vorbis_block_init",
    "vorbis_synthesis_restart", "vorbis_synthesis_halfrate", "vorbis_block_clear", "vorbis_dsp_clear", "vorbis_info_clear",
    "vorbis_synthesis_lapout", "ov_read 16-bit pack loop"
};
// Optional: trackonly (1.1+), halfrate (1.2+), lapout. Everything else must be there or nothing is hooked.
static const U32 hostVorbisRequired = (1u << VB_COUNT) - 1 - (1u << VB_TRACKONLY) - (1u << VB_HALFRATE) - (1u << VB_LAPOUT) - (1u << VB_PACK16);

U32 hostVorbisFnId(const char* name) { // VB_* + 1, or 0
    for (U32 i = 0; i < VB_COUNT; i++) {
        if (!strcmp(hostVorbisNames[i], name)) return i + 1;
    }
    return 0;
}

struct HostVorbisExport { U32 address; U32 fn; U8 head[8]; };
struct HostVorbisProcess {
    std::vector<U64> seen = std::vector<U64>(0x10000 / 64); // 64K regions already looked at
    std::vector<U32> images; // PE bases already parsed
    std::vector<HostVorbisExport> exports;
};
// shortcut: per pid, never freed when a process exits (a few KB each); a reused pid inherits the table, and the head
// bytes check keeps foreign code from being hooked.
static std::unordered_map<U32, HostVorbisProcess> hostVorbisProcesses;
static U8 hostVorbisPageOffsets[K_PAGE_SIZE / 8]; // bit per page offset of a hooked export: the cheap filter
static int hostVorbisState; // 0: not decided yet, 1: on, -1: off

// Guest bytes that are already RAM (never loads a page: this runs while a block is being decoded).
static bool vbPeek(KMemory* memory, U32 address, void* out, U32 len) {
    U8* to = (U8*)out;
    while (len) {
        if (address >> K_PAGE_SHIFT >= K_NUMBER_OF_PAGES) return false;
        MMU& mmu = getMemData(memory)->mmu[address >> K_PAGE_SHIFT];
        if (!mmu.canReadRam) return false;
        U32 n = std::min(len, K_PAGE_SIZE - (address & K_PAGE_MASK));
        ::memcpy(to, ramPageGet((RamPage)mmu.ramIndex) + (address & K_PAGE_MASK), n);
        to += n;
        address += n;
        len -= n;
        if (!address && len) return false; // wrapped
    }
    return true;
}

static U32 vbPeek32(KMemory* memory, U32 address, bool& ok) {
    U32 v = 0;
    if (!vbPeek(memory, address, &v, 4)) ok = false;
    return v;
}

// libvorbisfile's ov_read (ov_read_filter in 1.3.2+) converts the decoded floats to 16-bit samples in a loop that
// calls vorbis_ftoi (`fld qword; fistp dword`) per sample. Only one build is known, the one 最果てのイマ ships
// (MSVC 9, unoptimized, 2012-01-05; Muramasa's 2016 build is optimized and differs): its ov_read must match byte for
// byte and the signed, native-endian 16-bit loop inside ov_read_filter must too (but for the relocated address of
// its 32768.0 constant), as must the two helpers it calls. That loop is then hooked at its first block and run here
// on the frame's own locals; it ends at the loop's exit. Nothing upstream is assumed: whatever ov_read_filter left
// in the locals, the loop is done as the guest bytes would do it.
static const U8 vbOvRead[0x2d] = { // ov_read: push 0, push 0, push its 7 arguments, call ov_read_filter, add esp, 0x24
    0x55, 0x8b, 0xec, 0x6a, 0x00, 0x6a, 0x00, 0x8b, 0x45, 0x20, 0x50, 0x8b, 0x4d, 0x1c, 0x51, 0x8b, 0x55, 0x18, 0x52, 0x8b,
    0x45, 0x14, 0x50, 0x8b, 0x4d, 0x10, 0x51, 0x8b, 0x55, 0x0c, 0x52, 0x8b, 0x45, 0x08, 0x50, 0xe8, 0xc8, 0xf9, 0xff, 0xff,
    0x83, 0xc4, 0x24, 0x5d, 0xc3
};
enum : U32 { VBP_FROM_OV_READ = 0x42a, VBP_LEN = 0xbb, VBP_CONST = 0x67, VBP_FTOI = 0x3ba, VBP_SETROUND = 0x3da };
static const U8 vbPack16[VBP_LEN] = { // ov_read_filter + 0x1e6 up to the loop's exit (VBP_LEN); VBP_CONST: a relocated address
    0x8d, 0x55, 0xe0, 0x52, 0xe8, 0xd1, 0x03, 0x00, 0x00, 0x83, 0xc4, 0x04, 0xc7, 0x45, 0xf8, 0x00, 0x00, 0x00, 0x00, 0xeb,
    0x09, 0x8b, 0x45, 0xf8, 0x83, 0xc0, 0x01, 0x89, 0x45, 0xf8, 0x8b, 0x4d, 0xf8, 0x3b, 0x4d, 0xdc, 0x0f, 0x8d, 0x91, 0x00,
    0x00, 0x00, 0x8b, 0x55, 0xf8, 0x8b, 0x45, 0xfc, 0x8b, 0x0c, 0x90, 0x89, 0x4d, 0xc8, 0x8b, 0x55, 0xf8, 0x8b, 0x45, 0x0c,
    0x8d, 0x0c, 0x50, 0x89, 0x4d, 0xcc, 0xc7, 0x45, 0xec, 0x00, 0x00, 0x00, 0x00, 0xeb, 0x09, 0x8b, 0x55, 0xec, 0x83, 0xc2,
    0x01, 0x89, 0x55, 0xec, 0x8b, 0x45, 0xec, 0x3b, 0x45, 0xf4, 0x7d, 0x5a, 0x8b, 0x4d, 0xec, 0x8b, 0x55, 0xc8, 0xd9, 0x04,
    0x8a, 0xdc, 0x0d, 0x28, 0x52, 0x01, 0x10, 0x83, 0xec, 0x08, 0xdd, 0x1c, 0x24, 0xe8, 0x44, 0x03, 0x00, 0x00, 0x83, 0xc4,
    0x08, 0x89, 0x45, 0xd8, 0x81, 0x7d, 0xd8, 0xff, 0x7f, 0x00, 0x00, 0x7e, 0x09, 0xc7, 0x45, 0xd8, 0xff, 0x7f, 0x00, 0x00,
    0xeb, 0x10, 0x81, 0x7d, 0xd8, 0x00, 0x80, 0xff, 0xff, 0x7d, 0x07, 0xc7, 0x45, 0xd8, 0x00, 0x80, 0xff, 0xff, 0x8b, 0x45,
    0xcc, 0x66, 0x8b, 0x4d, 0xd8, 0x66, 0x89, 0x08, 0x8b, 0x55, 0xdc, 0x8b, 0x45, 0xcc, 0x8d, 0x0c, 0x50, 0x89, 0x4d, 0xcc,
    0xeb, 0x95, 0xe9, 0x5a, 0xff, 0xff, 0xff
};
static const U8 vbFtoi[] = { 0x55, 0x8b, 0xec, 0x51, 0xdd, 0x45, 0x08, 0xdb, 0x5d, 0xfc, 0x8b, 0x45, 0xfc, 0x8b, 0xe5, 0x5d, 0xc3 }; // fld qword [ebp+8]; fistp dword [ebp-4]
static const U8 vbSetround[] = { 0x55, 0x8b, 0xec, 0x5d, 0xc3 }; // vorbis_fpu_setround: nothing

// True (and e set) when ov_read at ovRead is the known build: e is then its pack loop.
static bool hostVorbisFileParse(KMemory* memory, U32 ovRead, HostVorbisExport& e) {
    U8 code[VBP_LEN];
    U32 loop = ovRead - VBP_FROM_OV_READ;
    if (!vbPeek(memory, ovRead, code, sizeof(vbOvRead)) || memcmp(code, vbOvRead, sizeof(vbOvRead))) return false;
    if (!vbPeek(memory, loop, code, VBP_LEN) || memcmp(code, vbPack16, VBP_CONST) ||
        memcmp(code + VBP_CONST + 4, vbPack16 + VBP_CONST + 4, VBP_LEN - VBP_CONST - 4)) return false;
    if (!vbPeek(memory, loop + VBP_FTOI, code, sizeof(vbFtoi)) || memcmp(code, vbFtoi, sizeof(vbFtoi))) return false;
    if (!vbPeek(memory, loop + VBP_SETROUND, code, sizeof(vbSetround)) || memcmp(code, vbSetround, sizeof(vbSetround))) return false;
    bool ok = true;
    double scale;
    if (!vbPeek(memory, vbPeek32(memory, loop + VBP_CONST, ok), &scale, 8) || !ok || scale != 32768.0) return false;
    e.address = loop;
    e.fn = VB_PACK16;
    return vbPeek(memory, loop, e.head, sizeof(e.head));
}

U32 hostVorbisFileLoop(KMemory* memory, U32 ovRead) { // for tests: the pack loop's address, or 0
    HostVorbisExport e;
    return hostVorbisFileParse(memory, ovRead, e) ? e.address : 0;
}

// Hooks the image at base when it exports the libvorbis decode functions (or libvorbisfile's ov_read).
static void hostVorbisParse(KMemory* memory, U32 base, HostVorbisProcess& p, U32 pid) {
    bool ok = true;
    U32 pe = base + vbPeek32(memory, base + 0x3c, ok);
    if (!ok || vbPeek32(memory, pe, ok) != 0x4550 || !ok) return; // "PE\0\0"
    U32 opt = pe + 24;
    U8 magic[2];
    if (!vbPeek(memory, opt, magic, 2) || magic[0] != 0x0b || magic[1] != 0x01) return; // PE32
    U32 exportRva = vbPeek32(memory, opt + 96, ok), exportSize = vbPeek32(memory, opt + 100, ok);
    if (!ok || !exportRva) return;
    U32 dir = base + exportRva;
    U32 count = vbPeek32(memory, dir + 24, ok), functions = base + vbPeek32(memory, dir + 28, ok);
    U32 names = base + vbPeek32(memory, dir + 32, ok), ordinals = base + vbPeek32(memory, dir + 36, ok);
    U32 functionCount = vbPeek32(memory, dir + 20, ok);
    if (!ok || count > 0x10000) return;
    std::vector<HostVorbisExport> found;
    U32 have = 0;
    for (U32 i = 0; i < count; i++) {
        char name[32] = {};
        U32 nameAddress = base + vbPeek32(memory, names + i * 4, ok);
        if (!ok) return;
        if (vbPeek(memory, nameAddress, name, 8) && !memcmp(name, "ov_read", 8)) { // libvorbisfile
            U16 ordinal;
            if (!vbPeek(memory, ordinals + i * 2, &ordinal, 2) || ordinal >= functionCount) return;
            U32 rva = vbPeek32(memory, functions + ordinal * 4, ok);
            if (!ok) return;
            HostVorbisExport e;
#ifdef __EMSCRIPTEN__
            if (vnboxUrlIs("nativeVorbisfile", "0")) {
                klog_fmt("[vnbox] native vorbisfile off (nativeVorbisfile=0)");
                return;
            }
#endif
            if (hostVorbisFileParse(memory, base + rva, e)) {
                p.exports.push_back(e);
                hostVorbisPageOffsets[(e.address & K_PAGE_MASK) >> 3] |= 1 << (e.address & 7);
                klog_fmt("[vnbox] native vorbis: libvorbisfile image at 0x%x in pid %d, ov_read's 16-bit pack loop at 0x%x", base, pid, e.address);
            } else {
                klog_fmt("[vnbox] native vorbis: libvorbisfile image at 0x%x isn't a known build, ov_read stays guest code", base);
            }
            return;
        }
        if (memcmp(name, "vorbis_", 7)) continue;
        vbPeek(memory, nameAddress, name, sizeof(name) - 1); // may stop short at a page end: then it just doesn't match
        if (!memchr(name, 0, sizeof(name) - 1)) continue;
        U32 fn = hostVorbisFnId(name);
        if (!fn) continue;
        U16 ordinal;
        if (!vbPeek(memory, ordinals + i * 2, &ordinal, 2) || ordinal >= functionCount) return;
        U32 rva = vbPeek32(memory, functions + ordinal * 4, ok);
        if (!ok) return;
        if (rva >= exportRva && rva - exportRva < exportSize) continue; // forwarder
        HostVorbisExport e = { base + rva, fn - 1, {} };
        if (!vbPeek(memory, e.address, e.head, sizeof(e.head))) continue;
        found.push_back(e);
        have |= 1u << e.fn;
    }
    if ((have & hostVorbisRequired) != hostVorbisRequired) {
        if (have) klog_fmt("[vnbox] native vorbis: image at 0x%x exports only some vorbis_* functions (0x%x), left to the guest", base, have);
        return;
    }
    for (const HostVorbisExport& e : found) {
        p.exports.push_back(e);
        hostVorbisPageOffsets[(e.address & K_PAGE_MASK) >> 3] |= 1 << (e.address & 7);
        if (memory->getDecodedOp(e.address)) klog_fmt("[vnbox] native vorbis: %s already decoded, stays guest code until invalidated", hostVorbisNames[e.fn]);
    }
    klog_fmt("[vnbox] native vorbis: libvorbis image at 0x%x in pid %d, %d functions", base, pid, (int)found.size());
}

// VB_* + 1 when the block at eip (linear) starts a hooked libvorbis export in this process, else 0.
U32 hostVorbisAt(CPU* cpu, U32 eip) {
#ifdef BOXEDWINE_HOST_VORBIS
    if (hostVorbisState < 0) return 0;
    if (!hostVorbisState) {
        hostVorbisState = 1;
#ifdef __EMSCRIPTEN__
        if (vnboxUrlIs("nativeVorbis", "0")) {
            hostVorbisState = -1;
            klog_fmt("[vnbox] native vorbis off (nativeVorbis=0)");
            return 0;
        }
#endif
    }
    U32 pid = cpu->thread->process->id;
    HostVorbisProcess& p = hostVorbisProcesses[pid];
    U32 region = eip >> 16;
    if (!(p.seen[region >> 6] & (1ull << (region & 63)))) {
        p.seen[region >> 6] |= 1ull << (region & 63);
        // Look down for the image holding eip: an unreadable or non-RAM region ends it (file-mapped code isn't RAM
        // until loaded, and is handled by hostLibcMapped anyway). Images over 16 MB aren't looked for.
        for (U32 base = region << 16, i = 0; i < 256; i++, base -= 0x10000) {
            U8 mz[2];
            if (!vbPeek(cpu->memory, base, mz, 2)) break;
            if (mz[0] == 'M' && mz[1] == 'Z') {
                if (std::find(p.images.begin(), p.images.end(), base) == p.images.end()) {
                    p.images.push_back(base);
                    hostVorbisParse(cpu->memory, base, p, pid);
                }
                break;
            }
            if (!base) break;
        }
    }
    if (!(hostVorbisPageOffsets[(eip & K_PAGE_MASK) >> 3] & (1 << (eip & 7)))) return 0;
    for (const HostVorbisExport& e : p.exports) {
        U8 head[sizeof(e.head)];
        if (e.address == eip && vbPeek(cpu->memory, eip, head, sizeof(head)) && !memcmp(head, e.head, sizeof(head))) return e.fn + 1;
    }
#endif
    return 0;
}

#ifdef BOXEDWINE_HOST_VORBIS
struct HostVorbisInfo { vorbis_info vi; vorbis_comment vc; bool ok = true; };
struct HostVorbisDsp { vorbis_dsp_state vd; U64 viKey; U32 guestVi; bool host = true, checked = false; };
struct HostVorbisBlock { vorbis_block vb; HostVorbisDsp* dsp; };
static std::unordered_map<U64, std::unique_ptr<HostVorbisInfo>> hostVorbisInfos; // pid << 32 | guest vi->codec_setup
static std::unordered_map<U64, std::unique_ptr<HostVorbisDsp>> hostVorbisDsps; // pid << 32 | guest vd
static std::unordered_map<U64, std::unique_ptr<HostVorbisBlock>> hostVorbisBlocks; // pid << 32 | guest vb

// Guest vorbis_dsp_state offsets (public, libvorbis 1.x, 32-bit with 8-aligned int64 like MSVC/MinGW).
enum : U32 { VI_CHANNELS = 4, VI_CODEC_SETUP = 0x1c, VD_VI = 4, VD_PCM = 8, VD_PCMRET = 0xc, VD_PCM_STORAGE = 0x10, VD_PCM_CURRENT = 0x14, VD_SIZE = 0x48 };

static void hostVorbisFreeBlock(decltype(hostVorbisBlocks)::iterator it) {
    vorbis_block_clear(&it->second->vb);
    hostVorbisBlocks.erase(it);
}

static void hostVorbisFreeDsp(decltype(hostVorbisDsps)::iterator it) {
    HostVorbisDsp* dsp = it->second.get();
    for (auto b = hostVorbisBlocks.begin(); b != hostVorbisBlocks.end();) {
        if (b->second->dsp == dsp) hostVorbisFreeBlock(b++);
        else ++b;
    }
    if (dsp->host) vorbis_dsp_clear(&dsp->vd);
    hostVorbisDsps.erase(it);
}

static void hostVorbisFreeInfo(U64 key) {
    auto it = hostVorbisInfos.find(key);
    if (it == hostVorbisInfos.end()) return;
    for (auto d = hostVorbisDsps.begin(); d != hostVorbisDsps.end();) {
        if (d->second->viKey == key) hostVorbisFreeDsp(d++);
        else ++d;
    }
    vorbis_comment_clear(&it->second->vc);
    vorbis_info_clear(&it->second->vi);
    hostVorbisInfos.erase(it);
}

// A guest ogg_packet (packet, bytes, b_o_s, e_o_s, int64 granulepos at 0x10, int64 packetno) as a host one over a copy.
static bool hostVorbisPacket(KMemory* memory, U32 address, ogg_packet& op, std::vector<U8>& data) {
    if (!memory->canRead(address, 32)) return false;
    U32 packet = memory->readd(address), bytes = memory->readd(address + 4);
    if (bytes > 0x1000000 || !memory->canRead(packet, bytes)) return false;
    data.resize(bytes);
    memory->memcpy(data.data(), packet, bytes);
    op.packet = data.data();
    op.bytes = (long)bytes;
    op.b_o_s = (long)memory->readd(address + 8);
    op.e_o_s = (long)memory->readd(address + 12);
    op.granulepos = (ogg_int64_t)memory->readq(address + 16);
    op.packetno = (ogg_int64_t)memory->readq(address + 24);
    return true;
}

// The guest vd's public fields from pcm_current on, as the host's.
static void hostVorbisSync(KMemory* memory, U32 vd, const vorbis_dsp_state& v) {
    const U32 fields[] = { (U32)v.pcm_current, (U32)v.pcm_returned, (U32)v.preextrapolate, (U32)v.eofflag,
        (U32)v.lW, (U32)v.W, (U32)v.nW, (U32)v.centerW };
    for (U32 i = 0; i < 8; i++) memory->writed(vd + VD_PCM_CURRENT + i * 4, fields[i]);
    memory->writeq(vd + 0x38, (U64)v.granulepos);
    memory->writeq(vd + 0x40, (U64)v.sequence);
}
#endif

// ov_read_filter's signed native-endian 16-bit loop (vbPack16), from its first block to its exit, on the frame's
// locals: for i < channels [ebp-0x24]: src = pcm [ebp-4][i], dest = buffer [ebp+0xc] + i*2; for j < samples [ebp-0xc]:
// val = fistp(src[j] * 32768.0) in the guest's rounding mode, clamped to 16 bits, *dest = val, dest += channels*2.
// The locals end as the guest leaves them; registers, flags and the stack below ESP are dead at the exit (it reloads
// them), the x87 stack is balanced. Anything odd (unreadable, overlapping the frame or the sources) is left to the guest.
#ifdef BOXEDWINE_HOST_VORBIS
static bool hostVorbisPack16(CPU* cpu) {
    KMemory* memory = cpu->memory;
    U32 eip = cpu->eip.u32, ebp = EBP;
    if (!memory->canRead(eip + VBP_CONST, 4) || !memory->canRead(ebp - 0x40, 0x50) || !memory->canWrite(ebp - 0x40, 0x40)) return false;
    double scale = 0;
    U32 scaleAddress = memory->readd(eip + VBP_CONST);
    if (memory->canRead(scaleAddress, 8)) {
        U64 bits = memory->readq(scaleAddress);
        ::memcpy(&scale, &bits, 8);
    }
    if (scale != 32768.0) return false; // what fld float * 32768.0 then fstp double is exactly, at any x87 precision
    S32 channels = (S32)memory->readd(ebp - 0x24), samples = (S32)memory->readd(ebp - 0xc);
    U32 pcm = memory->readd(ebp - 4), buffer = memory->readd(ebp + 0xc);
    memory->writed(ebp - 8, 0); // i
    if (channels <= 0) {
        cpu->eip.u32 = eip + VBP_LEN;
        return true;
    }
    if (channels > 255 || samples > 0x100000) return false;
    U32 n = samples > 0 ? (U32)samples : 0, bytes = n * (U32)channels * 2;
    auto overlaps = [&](U32 a, U32 alen) { return bytes && a < buffer + bytes && buffer < a + alen; };
    if (!memory->canRead(pcm, (U32)channels * 4) || (bytes && (buffer + bytes < buffer || !memory->canWrite(buffer, bytes))) ||
        overlaps(ebp - 0x40, 0x50) || overlaps(pcm, (U32)channels * 4)) return false;
    static thread_local std::vector<float> src;
    static thread_local std::vector<S16> out;
    src.resize(n);
    out.resize((size_t)n * channels);
    FPU& fpu = cpu->fpu;
    S32 val = (S32)memory->readd(ebp - 0x28);
    for (S32 i = 0; i < channels; i++) {
        U32 from = memory->readd(pcm + i * 4);
        if (n && (from + n * 4 < from || !memory->canRead(from, n * 4) || overlaps(from, n * 4))) return false; // nothing written yet but i
        if (n) memory->memcpy(src.data(), from, n * 4);
        for (U32 j = 0; j < n; j++) {
            double r = fpu.FROUND((double)src[j] * 32768.0); // as the emulated fistp: the control word's rounding
            val = r >= -2147483648.0 && r < 2147483648.0 ? (S32)r : (S32)0x80000000; // NaN, out of range: integer indefinite
            if (val > 32767) val = 32767;
            else if (val < -32768) val = -32768;
            out[(size_t)j * channels + i] = (S16)val;
        }
    }
    if (bytes) memory->memcpy(buffer, out.data(), bytes);
    memory->writed(ebp - 8, (U32)channels);
    memory->writed(ebp - 0x14, samples > 0 ? (U32)samples : 0);
    memory->writed(ebp - 0x38, memory->readd(pcm + (channels - 1) * 4));
    memory->writed(ebp - 0x34, buffer + (channels - 1) * 2 + bytes);
    memory->writed(ebp - 0x28, (U32)val);
    cpu->eip.u32 = eip + VBP_LEN;
    return true;
}
#endif

// Runs the hooked libvorbis export fn (VB_*) for the call the CPU is at (cdecl). True when the call was done here
// (EAX set, returned); false runs the guest function, after any mirrored side effect.
bool hostVorbisRun(CPU* cpu, U32 fn) {
#ifdef BOXEDWINE_HOST_VORBIS
    KMemory* memory = cpu->memory;
    // A debug trap only matters at a dsp state's first host-only call (below). A stream on the host stays there, since the
    // guest's state was never advanced, and the mirrored calls have no guest effect. shortcut: host writes don't fire a watchpoint.
    if (!cpu->isBig() || cpu->stackMask != 0xFFFFFFFF || cpu->seg[CS].address || cpu->seg[DS].address ||
        cpu->seg[ES].address || cpu->seg[SS].address || !memory->canRead(ESP, 16)) return false;
    if (fn == VB_PACK16) return hostVorbisPack16(cpu);
    U64 pid = (U64)cpu->thread->process->id << 32;
    U32 a0 = memory->readd(ESP + 4), a1 = memory->readd(ESP + 8), a2 = memory->readd(ESP + 12);
    auto infoKey = [&](U32 vi) -> U64 { // 0 when the guest vi isn't readable or has no codec_setup
        U32 setup = vi && memory->canRead(vi + VI_CODEC_SETUP, 4) ? memory->readd(vi + VI_CODEC_SETUP) : 0;
        return setup ? pid | setup : 0;
    };
    static thread_local std::vector<U8> data;
    ogg_packet op;
    S32 result = 0;
    switch (fn) {
    case VB_HEADERIN: { // (vi, vc, op)
        U64 key = infoKey(a0);
        if (!key) return false;
        if (!hostVorbisPacket(memory, a2, op, data)) {
            hostVorbisFreeInfo(key); // can't follow this vi any more
            return false;
        }
        auto it = hostVorbisInfos.find(key);
        if (op.bytes >= 7 && op.packet[0] == 1 && !memcmp(op.packet + 1, "vorbis", 6)) { // id header: a new stream
            if (it != hostVorbisInfos.end()) hostVorbisFreeInfo(key);
            auto info = std::make_unique<HostVorbisInfo>();
            vorbis_info_init(&info->vi);
            vorbis_comment_init(&info->vc);
            it = hostVorbisInfos.emplace(key, std::move(info)).first;
        }
        if (it != hostVorbisInfos.end() && it->second->ok && vorbis_synthesis_headerin(&it->second->vi, &it->second->vc, &op)) it->second->ok = false;
        return false;
    }
    case VB_SYNTHESIS_INIT: { // (vd, vi)
        auto old = hostVorbisDsps.find(pid | a0);
        if (old != hostVorbisDsps.end()) hostVorbisFreeDsp(old);
        U64 key = infoKey(a1);
        auto info = hostVorbisInfos.find(key);
        if (!key || info == hostVorbisInfos.end() || !info->second->ok) {
            klog_fmt("[vnbox] native vorbis: vd %x init with vi %x whose headers weren't followed, decoded by the guest", a0, a1);
            return false;
        }
        auto dsp = std::make_unique<HostVorbisDsp>();
        dsp->viKey = key;
        dsp->guestVi = a1;
        if (vorbis_synthesis_init(&dsp->vd, &info->second->vi)) return false;
        hostVorbisDsps.emplace(pid | a0, std::move(dsp));
        return false;
    }
    case VB_BLOCK_INIT: { // (vd, vb)
        auto old = hostVorbisBlocks.find(pid | a1);
        if (old != hostVorbisBlocks.end()) hostVorbisFreeBlock(old);
        auto dsp = hostVorbisDsps.find(pid | a0);
        if (dsp == hostVorbisDsps.end() || !dsp->second->host) return false;
        auto block = std::make_unique<HostVorbisBlock>();
        block->dsp = dsp->second.get();
        vorbis_block_init(&block->dsp->vd, &block->vb);
        hostVorbisBlocks.emplace(pid | a1, std::move(block));
        return false;
    }
    case VB_RESTART: { // (vd)
        auto dsp = hostVorbisDsps.find(pid | a0);
        if (dsp != hostVorbisDsps.end() && dsp->second->host) vorbis_synthesis_restart(&dsp->second->vd);
        return false;
    }
    case VB_HALFRATE: { // (vi, flag)
        auto info = hostVorbisInfos.find(infoKey(a0));
        if (info != hostVorbisInfos.end() && info->second->ok) vorbis_synthesis_halfrate(&info->second->vi, (int)a1);
        return false;
    }
    case VB_BLOCK_CLEAR: {
        auto it = hostVorbisBlocks.find(pid | a0);
        if (it != hostVorbisBlocks.end()) hostVorbisFreeBlock(it);
        return false;
    }
    case VB_DSP_CLEAR: {
        auto it = hostVorbisDsps.find(pid | a0);
        if (it != hostVorbisDsps.end()) hostVorbisFreeDsp(it);
        return false;
    }
    case VB_INFO_CLEAR:
        if (U64 key = infoKey(a0)) hostVorbisFreeInfo(key);
        return false;
    case VB_LAPOUT: {
        static bool logged;
        if (!logged) klog_fmt("[vnbox] native vorbis: vorbis_synthesis_lapout called (ov_crosslap?), its samples may be wrong");
        logged = true;
        return false;
    }
    case VB_SYNTHESIS:
    case VB_TRACKONLY: { // (vb, op)
        auto block = hostVorbisBlocks.find(pid | a0);
        if (block == hostVorbisBlocks.end()) return false;
        HostVorbisDsp* dsp = block->second->dsp;
        if (!dsp->checked) { // the first host-only call of this dsp state: nothing has diverged yet
            U32 vd = 0;
            for (auto& d : hostVorbisDsps) if (d.second.get() == dsp) vd = (U32)d.first;
            const vorbis_info* vi = dsp->vd.vi;
            bool ok = !cpu->debugTrapActive && vd && memory->canRead(vd, VD_SIZE) && memory->canWrite(vd, VD_SIZE) && memory->readd(vd + VD_VI) == dsp->guestVi &&
                memory->readd(vd + VD_PCM_STORAGE) == (U32)dsp->vd.pcm_storage && infoKey(dsp->guestVi) == dsp->viKey &&
                memory->readd(dsp->guestVi + VI_CHANNELS) == (U32)vi->channels;
            klog_fmt("[vnbox] native vorbis: stream at vd %x (%d ch, %ld Hz) decoded by the %s", vd, vi->channels, vi->rate, ok ? "host" : "guest");
            if (!ok) {
                vorbis_dsp_clear(&dsp->vd);
                dsp->host = false;
                for (auto b = hostVorbisBlocks.begin(); b != hostVorbisBlocks.end();) {
                    if (b->second->dsp == dsp) hostVorbisFreeBlock(b++);
                    else ++b;
                }
                return false;
            }
            dsp->checked = true;
        }
        if (!hostVorbisPacket(memory, a1, op, data)) {
            result = OV_EFAULT; // shortcut: an unreadable packet returns OV_EFAULT where the guest would fault; never seen.
        } else {
            result = fn == VB_SYNTHESIS ? vorbis_synthesis(&block->second->vb, &op) : vorbis_synthesis_trackonly(&block->second->vb, &op);
        }
        break;
    }
    case VB_BLOCKIN: { // (vd, vb)
        auto dsp = hostVorbisDsps.find(pid | a0);
        auto block = hostVorbisBlocks.find(pid | a1);
        if (dsp == hostVorbisDsps.end() || !dsp->second->host || !dsp->second->checked) return false;
        if (block == hostVorbisBlocks.end() || block->second->dsp != dsp->second.get()) {
            result = OV_EINVAL; // shortcut: a block from another dsp state; libvorbis itself would mix them up.
            break;
        }
        result = vorbis_synthesis_blockin(&dsp->second->vd, &block->second->vb);
        hostVorbisSync(memory, a0, dsp->second->vd);
        break;
    }
    case VB_PCMOUT: { // (vd, float*** pcm)
        auto dsp = hostVorbisDsps.find(pid | a0);
        if (dsp == hostVorbisDsps.end() || !dsp->second->host || !dsp->second->checked) return false;
        vorbis_dsp_state& v = dsp->second->vd;
        U32 returned = (U32)v.pcm_returned;
        float** pcm;
        result = vorbis_synthesis_pcmout(&v, a1 ? &pcm : nullptr);
        if (a1 && result > 0) {
            U32 channels = (U32)v.vi->channels, gpcm = memory->readd(a0 + VD_PCM), gret = memory->readd(a0 + VD_PCMRET);
            bool ok = memory->canWrite(a1, 4) && memory->canRead(gpcm, channels * 4) && memory->canWrite(gret, channels * 4);
            for (U32 ch = 0; ok && ch < channels; ch++) ok = memory->canWrite(memory->readd(gpcm + ch * 4) + returned * 4, (U32)result * 4);
            if (!ok) {
                result = 0; // shortcut: unwritable guest buffers give no samples where the guest would fault; never seen.
                break;
            }
            for (U32 ch = 0; ch < channels; ch++) {
                U32 to = memory->readd(gpcm + ch * 4) + returned * 4;
                memory->memcpy(to, pcm[ch], (U32)result * 4);
                memory->writed(gret + ch * 4, to);
            }
            memory->writed(a1, gret);
        }
        break;
    }
    case VB_READ: { // (vd, samples)
        auto dsp = hostVorbisDsps.find(pid | a0);
        if (dsp == hostVorbisDsps.end() || !dsp->second->host || !dsp->second->checked) return false;
        result = vorbis_synthesis_read(&dsp->second->vd, (int)a1);
        hostVorbisSync(memory, a0, dsp->second->vd);
        break;
    }
    default:
        return false;
    }
    EAX = (U32)result;
    cpu->eip.u32 = cpu->pop32();
    return true;
#else
    return false;
#endif
}
