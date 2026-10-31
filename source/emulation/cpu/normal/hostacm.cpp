/*
 *  Copyright (C) 2012-2025  The BoxedWine Team
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

// Native Vorbis ACM: games that ship H. Mutsuki's vorbis.acm (Ogg Vorbis ACM codec, MSVC7 + libvorbis 1.0, all
// x87) decode their BGM through it, emulated. Its DriverProc export gets a head op (hostLibcOp, the native libc
// mechanism in hosthooks.cpp) that runs ACMDM_STREAM_CONVERT for decode streams in C++ with libogg/libvorbis
// (Emscripten ports); every other message, and every stream it doesn't take, runs the guest code.
//
// It mirrors the driver's own convert (the one build whose .text hash is below; other builds aren't hooked):
// the whole source buffer is always consumed (fed to ogg_sync in 4096-byte pieces, decoding a page whenever one is
// complete), cbSrcLengthUsed / cbDstLengthUsed are added to (not set), output stops when the destination is full and
// the rest of a decoded block waits for the next call, ACM_STREAMCONVERTF_END lets one more page through and then
// ends the stream, START/BLOCKALIGN and RESET (not implemented by the driver) change nothing. Samples are
// trunc(x * 32767) clamped, as its x87 loop does. Differences: the driver builds the Vorbis headers from the
// format (mode 3) and skips the stream's own; the host decoder reads the stream's headers, so a stream is only
// taken when its first convert holds all three valid headers matching the format. libvorbis 1.3.7 vs 1.0 may
// round samples differently.
//
// A stream is decided at its first convert, before anything is written: taken (host from then on) or left to
// the guest for its whole life. ACMDM_STREAM_CLOSE frees the host state, then runs the guest's close.
// nativeAcm=0 (URL) turns it off.
#ifdef BOXEDWINE_HOST_VORBIS
#include <ogg/ogg.h>
#include <vorbis/codec.h>
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

static const U64 HOST_ACM_TEXT_HASH = 0xd5578a8339d19269ull; // FNV-1a 64 of vorbis.acm's .text (信天翁航海録's copy)

struct HostAcmModule { BString path; U32 driverProc; }; // file offset of DriverProc's first byte
static std::vector<HostAcmModule> hostAcmModules;
static std::vector<BString> hostAcmSeen; // vorbis.acm paths already looked at

static U32 acm16(const U8* p) { return p[0] | (p[1] << 8); }
static U32 acm32(const U8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((U32)p[3] << 24); }

// File offset of the DriverProc export when the file is the known vorbis.acm build, else 0 (host-side reads).
static U32 hostAcmParse(KFile* file) {
    auto read = [file](U64 offset, std::vector<U8>& buf, U32 len) {
        buf.resize(len);
        return file->preadNative(buf.data(), (S64)offset, len) == len;
    };
    std::vector<U8> mz, pe, sections, exports, text;
    if (!read(0, mz, 64) || mz[0] != 'M' || mz[1] != 'Z') return 0;
    U32 peOffset = acm32(&mz[0x3c]);
    if (!read(peOffset, pe, 24 + 96 + 16) || memcmp(pe.data(), "PE\0\0", 4) || acm16(&pe[4]) != 0x14c) return 0;
    U32 count = acm16(&pe[6]), optSize = acm16(&pe[20]);
    if (optSize < 96 + 8 || !read(peOffset + 24 + optSize, sections, count * 40)) return 0;
    U32 exportRva = acm32(&pe[24 + 96]);
    auto fileOffset = [&](U32 rva) -> U32 {
        for (U32 i = 0; i < count; i++) {
            const U8* s = &sections[i * 40];
            U32 va = acm32(s + 12), size = acm32(s + 16);
            if (rva >= va && rva - va < size) return rva - va + acm32(s + 20);
        }
        return 0;
    };
    U64 hash = 0xcbf29ce484222325ull;
    bool haveText = false;
    for (U32 i = 0; i < count && !haveText; i++) {
        const U8* s = &sections[i * 40];
        if (memcmp(s, ".text\0\0\0", 8) || !read(acm32(s + 20), text, acm32(s + 16))) continue;
        for (U8 b : text) hash = (hash ^ b) * 0x100000001b3ull;
        haveText = true;
    }
    if (!haveText || hash != HOST_ACM_TEXT_HASH) {
        klog_fmt("[vnbox] native acm: %s is not the known vorbis.acm build, left to the guest", file->openFile->node->path.c_str());
        return 0;
    }
    U32 dir = fileOffset(exportRva);
    if (!dir || !read(dir, exports, 40) || !acm32(&exports[20])) return 0;
    U32 functions = fileOffset(acm32(&exports[28]));
    std::vector<U8> first;
    if (!functions || !read(functions, first, 4)) return 0;
    return fileOffset(acm32(first.data())); // ordinal 1, the only export: DriverProc
}

// Called for every file mapping (hosthooks.cpp); true with DriverProc's file offset the first time a known
// vorbis.acm is mapped.
bool hostAcmMapped(KFile* file, U32& driverProc) {
    const BString& path = file->openFile->node->path;
    if (!path.endsWith("/vorbis.acm", true)) return false;
    for (const BString& seen : hostAcmSeen) {
        if (seen == path) return false;
    }
    hostAcmSeen.push_back(path);
#ifdef BOXEDWINE_HOST_VORBIS
#ifdef __EMSCRIPTEN__
    if (EM_ASM_INT({ return typeof vnboxParam === "function" && vnboxParam("nativeAcm") === "0" ? 1 : 0; })) {
        klog_fmt("[vnbox] native acm off (nativeAcm=0)");
        return false;
    }
#endif
    driverProc = hostAcmParse(file);
    if (!driverProc) return false;
    hostAcmModules.push_back({ path, driverProc });
    klog_fmt("[vnbox] native acm: %s, DriverProc at file offset 0x%x", path.c_str(), driverProc);
    return true;
#else
    return false;
#endif
}

bool hostAcmAt(const BString& path, U32 fileOffset) {
    for (const HostAcmModule& m : hostAcmModules) {
        if (m.driverProc == fileOffset && m.path == path) return true;
    }
    return false;
}

#ifdef BOXEDWINE_HOST_VORBIS
struct HostAcmStream {
    bool host = false; // false: left to the guest decoder for its whole life
    ogg_sync_state oy;
    ogg_stream_state os;
    vorbis_info vi;
    vorbis_comment vc;
    vorbis_dsp_state vd;
    vorbis_block vb;
    bool streamInit = false, decoding = false, endSeen = false, eos = false;
    int headers = 0;
    U32 pcmDone = 0; // bytes of the current decoded block already written (the driver's own counter)
    ~HostAcmStream() {
        if (!host) return;
        if (decoding) {
            vorbis_block_clear(&vb);
            vorbis_dsp_clear(&vd);
        }
        vorbis_comment_clear(&vc);
        vorbis_info_clear(&vi);
        if (streamInit) ogg_stream_clear(&os);
        ogg_sync_clear(&oy);
    }
};
static std::unordered_map<U64, std::unique_ptr<HostAcmStream>> hostAcmStreams; // pid << 32 | the driver's stream object

// True when buf starts with an Ogg stream whose first three packets are valid Vorbis headers for that format.
static bool hostAcmHeadersOk(const U8* buf, U32 len, U32 channels, U32 rate) {
    ogg_sync_state oy;
    ogg_stream_state os;
    vorbis_info vi;
    vorbis_comment vc;
    ogg_page og;
    ogg_packet op;
    ogg_sync_init(&oy);
    vorbis_info_init(&vi);
    vorbis_comment_init(&vc);
    ::memcpy(ogg_sync_buffer(&oy, len), buf, len);
    ogg_sync_wrote(&oy, len);
    int headers = 0;
    bool streamInit = false, ok = true;
    while (ok && headers < 3 && ogg_sync_pageout(&oy, &og) == 1) {
        if (!streamInit) {
            ok = ogg_page_bos(&og) && !ogg_stream_init(&os, ogg_page_serialno(&og));
            streamInit = ok;
        }
        if (ok && ogg_stream_pagein(&os, &og)) ok = false;
        int r;
        while (ok && headers < 3 && (r = ogg_stream_packetout(&os, &op)) != 0) {
            ok = r > 0 && vorbis_synthesis_headerin(&vi, &vc, &op) == 0;
            headers++;
        }
    }
    ok = ok && headers == 3 && vi.channels == (int)channels && vi.rate == (long)rate;
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    if (streamInit) ogg_stream_clear(&os);
    ogg_sync_clear(&oy);
    return ok;
}

// The driver's convert (decode streams) on host copies of the buffers; srcUsed/dstUsed are the header's fields.
static void hostAcmConvert(HostAcmStream& s, const U8* src, U32 srcLen, U32& srcUsed, U8* dst, U32 dstLen, U32& dstUsed, U32 align, bool end) {
    ogg_page og;
    ogg_packet op;
    auto feed = [&]() -> U32 {
        U32 n = std::min(4096u, srcLen - srcUsed);
        char* p = ogg_sync_buffer(&s.oy, 4096);
        ::memcpy(p, src + srcUsed, n);
        ogg_sync_wrote(&s.oy, n);
        srcUsed += n;
        return n;
    };
    // Writes what fits of the decoded block; its count when the block was used up, 0 when nothing was written.
    auto pcmout = [&]() -> U32 {
        float** pcm;
        int avail = vorbis_synthesis_pcmout(&s.vd, &pcm);
        if (avail <= 0) return 0;
        U32 already = s.pcmDone / align;
        U32 n = std::min((U32)avail - already, (dstLen - dstUsed) / align);
        for (int ch = 0; ch < s.vi.channels; ch++) {
            U8* out = dst + dstUsed + ch * 2;
            for (U32 i = 0; i < n; i++, out += align) {
                double v = (double)pcm[ch][already + i] * 32767.0;
                S32 x = v < 2147483648.0 && v >= -2147483648.0 ? (S32)v : (S32)0x80000000; // _ftol2: truncate, integer indefinite
                x = x >= 0x7fff ? 0x7fff : x < -0x8000 ? -0x8000 : x;
                out[0] = (U8)x;
                out[1] = (U8)(x >> 8);
            }
        }
        s.pcmDone += n * align;
        dstUsed += n * align;
        if (!n) return 0;
        if (already + n < (U32)avail) return n;
        s.pcmDone = 0;
        vorbis_synthesis_read(&s.vd, avail);
        return avail;
    };
    // Packets of the current page, then pcm; false when the destination is full.
    auto packets = [&]() -> bool {
        for (;;) {
            int r = ogg_stream_packetout(&s.os, &op);
            if (!r) return true;
            if (r < 0) continue;
            if (s.headers < 3) { // checked at the stream's first convert
                vorbis_synthesis_headerin(&s.vi, &s.vc, &op);
                if (++s.headers == 3) {
                    vorbis_synthesis_init(&s.vd, &s.vi);
                    vorbis_block_init(&s.vd, &s.vb);
                    s.decoding = true;
                }
                continue;
            }
            if (!s.decoding) continue;
            if (!vorbis_synthesis(&s.vb, &op)) vorbis_synthesis_blockin(&s.vd, &s.vb);
            U32 r2;
            do {
                r2 = pcmout();
                if (dstUsed == dstLen) return false;
            } while (r2);
        }
    };
    while (!s.eos) {
        if (s.pcmDone) {
            U32 r;
            bool full = false;
            do {
                r = pcmout();
                if (dstUsed == dstLen) full = true;
            } while (!full && r);
            if (full || !packets()) break;
            continue;
        }
        if (s.endSeen) s.eos = true;
        if (ogg_sync_pageout(&s.oy, &og) == 1) {
            if (!s.streamInit) {
                ogg_stream_init(&s.os, ogg_page_serialno(&og));
                s.streamInit = true;
            }
            ogg_stream_pagein(&s.os, &og);
            if (!packets()) break;
            continue;
        }
        if (feed()) continue;
        if (!end) break;
        s.endSeen = true;
    }
    while (srcUsed < srcLen) feed();
}
#endif

// DriverProc(dwDriverId, hDriver, uMsg, lParam1, lParam2), stdcall, at its first instruction. True when the call
// was done here (EAX set, returned with ret 0x14); false (nothing written) runs the guest's DriverProc.
bool hostAcmRun(CPU* cpu) {
#ifdef BOXEDWINE_HOST_VORBIS
    enum : U32 { ACMDM_STREAM_CLOSE = 0x604d, ACMDM_STREAM_CONVERT = 0x604f, WAVE_FORMAT_PCM = 1, VORBIS_MODE3PLUS = 0x6771 };
    KMemory* memory = cpu->memory;
    if (cpu->debugTrapActive || !cpu->isBig() || cpu->stackMask != 0xFFFFFFFF || cpu->seg[CS].address || cpu->seg[DS].address ||
        cpu->seg[ES].address || cpu->seg[SS].address || !memory->canRead(ESP, 24)) return false;
    U32 msg = memory->readd(ESP + 12), inst = memory->readd(ESP + 16), hdr = memory->readd(ESP + 20);
    if ((msg != ACMDM_STREAM_CONVERT && msg != ACMDM_STREAM_CLOSE) || !memory->canRead(inst, 40)) return false;
    U32 fdwDriver = memory->readd(inst + 28), object = memory->readd(inst + 32);
    U64 key = ((U64)cpu->thread->process->id << 32) | object;
    auto it = hostAcmStreams.find(key);
    if (msg == ACMDM_STREAM_CLOSE) {
        if (it != hostAcmStreams.end()) hostAcmStreams.erase(it);
        return false; // the guest frees its own (unused) decoder
    }
    if (fdwDriver != 2 || !object) return false; // 2: the driver's decode stream
    if (it != hostAcmStreams.end() && !it->second->host) return false;
    if (!memory->canRead(hdr, 48) || !memory->canWrite(hdr, 48)) return false;
    U32 pbSrc = memory->readd(hdr + 12), srcLen = memory->readd(hdr + 16), srcUsed = memory->readd(hdr + 20);
    U32 pbDst = memory->readd(hdr + 28), dstLen = memory->readd(hdr + 32), dstUsed = memory->readd(hdr + 36);
    U32 flags = memory->readd(hdr + 44);
    U32 wfxSrc = memory->readd(inst + 4), wfxDst = memory->readd(inst + 8);
    if (!memory->canRead(wfxSrc, 16) || !memory->canRead(wfxDst, 16)) return false;
    U32 channels = memory->readw(wfxSrc + 2), align = (memory->readw(wfxDst + 14) * memory->readw(wfxDst + 2)) / 8;
    bool buffersOk = srcUsed <= srcLen && dstUsed <= dstLen && memory->canRead(pbSrc + srcUsed, srcLen - srcUsed) &&
        memory->canWrite(pbDst + dstUsed, dstLen - dstUsed) && (U64)pbSrc + srcLen <= 0x100000000ull && (U64)pbDst + dstLen <= 0x100000000ull;
    static thread_local std::vector<U8> src, dst;
    src.resize(srcLen - std::min(srcUsed, srcLen));
    if (buffersOk) memory->memcpy(src.data(), pbSrc + srcUsed, (U32)src.size());
    if (it == hostAcmStreams.end()) { // first convert of this stream: take it or leave it to the guest for good
        bool take = buffersOk && memory->readw(wfxSrc) == VORBIS_MODE3PLUS && memory->readw(wfxDst) == WAVE_FORMAT_PCM &&
            memory->readw(wfxDst + 14) == 16 && memory->readw(wfxDst + 2) == channels && channels &&
            hostAcmHeadersOk(src.data(), (U32)src.size(), channels, memory->readd(wfxSrc + 4));
        auto stream = std::make_unique<HostAcmStream>();
        if (take) {
            stream->host = true;
            ogg_sync_init(&stream->oy);
            vorbis_info_init(&stream->vi);
            vorbis_comment_init(&stream->vc);
        }
        klog_fmt("[vnbox] native acm: stream %x decoded by the %s", object, take ? "host" : "guest");
        it = hostAcmStreams.emplace(key, std::move(stream)).first;
        if (!take) return false;
    }
    U32 result = 0;
    if (buffersOk) {
        // shortcut: an owned stream given a bad buffer returns MMSYSERR_INVALPARAM where the guest would fault; never seen.
        U32 used = 0, produced = 0;
        dst.resize(dstLen - dstUsed);
        hostAcmConvert(*it->second, src.data(), (U32)src.size(), used, dst.data(), (U32)dst.size(), produced, align, (flags & 0x20) != 0);
        memory->memcpy(pbDst + dstUsed, dst.data(), produced);
        memory->checkDebugTrapOnMemoryWrite(pbDst + dstUsed, produced);
        memory->writed(hdr + 20, srcUsed + used);
        memory->writed(hdr + 36, dstUsed + produced);
    } else {
        result = 11;
    }
    EAX = result;
    cpu->eip.u32 = cpu->pop32();
    ESP += 20;
    return true;
#else
    return false;
#endif
}
