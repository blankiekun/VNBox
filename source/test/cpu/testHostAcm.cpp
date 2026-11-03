/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "boxedwine.h"

#if defined(__TEST) && defined(BOXEDWINE_HOST_VORBIS)

// Native vorbis.acm (hostacm.cpp): ACMDM_STREAM_CONVERT on a host-decoded stream must give the same PCM however
// the game splits its buffers (the rest of a decoded block waits for the next call), consume all of every source
// buffer, add to cbSrcLengthUsed/cbDstLengthUsed, and return like DriverProc's `ret 0x14`; a stream it doesn't
// take at its first convert stays with the guest for good.
#include <ogg/ogg.h>
#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>
#include "testHostAcm.h"
#include "testCPU.h"
#include "../../io/fsmemnode.h"
#include "../../io/fsmemopennode.h"

bool hostAcmRun(CPU* cpu);
bool hostAcmMapped(KFile* file, U32& driverProc);

#define cpu (testContext().cpu)
#define memory (testContext().memory)

namespace {

constexpr U32 HEAP = 0xF0000000; // 32 RW pages (testCPU.cpp)
constexpr U32 STACK = HEAP + 0x400, INST = HEAP + 0x500, HDR = HEAP + 0x600, WFX_SRC = HEAP + 0x700, WFX_DST = HEAP + 0x740;
constexpr U32 SRC = HEAP + 0x1000, DST = HEAP + 0x10000, RET = 0x12345678;
constexpr U32 ACM_END = 0x20, ACM_INVALPARAM = 11; // ACM_STREAMCONVERTF_END, MMSYSERR_INVALPARAM

// 0.25 s of a stereo 44.1 kHz tone as an Ogg Vorbis stream (headers on their own pages, as encoders write them).
// headerBytes: when given, every audio packet gets its own page (so a convert can be handed exactly one more packet)
// and this is where the audio pages start.
std::vector<U8> encode(U32* headerBytes = nullptr) {
    vorbis_info vi;
    vorbis_comment vc;
    vorbis_dsp_state vd;
    vorbis_block vb;
    ogg_stream_state os;
    ogg_page og;
    ogg_packet op, h1, h2, h3;
    std::vector<U8> out;
    auto put = [&](ogg_page& p) {
        out.insert(out.end(), p.header, p.header + p.header_len);
        out.insert(out.end(), p.body, p.body + p.body_len);
    };
    vorbis_info_init(&vi);
    vorbis_encode_init_vbr(&vi, 2, 44100, 0.1f);
    vorbis_comment_init(&vc);
    vorbis_analysis_init(&vd, &vi);
    vorbis_block_init(&vd, &vb);
    ogg_stream_init(&os, 1);
    vorbis_analysis_headerout(&vd, &vc, &h1, &h2, &h3);
    ogg_stream_packetin(&os, &h1);
    ogg_stream_packetin(&os, &h2);
    ogg_stream_packetin(&os, &h3);
    while (ogg_stream_flush(&os, &og)) put(og);
    if (headerBytes) *headerBytes = (U32)out.size();
    for (int done = 0, n = 1; n; done += n) {
        n = std::min(1024, 11025 - done); // the last round (n = 0) ends the stream
        if (n) {
            float** buf = vorbis_analysis_buffer(&vd, n);
            for (int i = 0; i < n; i++) buf[0][i] = buf[1][i] = 0.5f * sinf((done + i) * 0.0627f);
        }
        vorbis_analysis_wrote(&vd, n);
        while (vorbis_analysis_blockout(&vd, &vb) == 1) {
            vorbis_analysis(&vb, nullptr);
            vorbis_bitrate_addblock(&vb);
            while (vorbis_bitrate_flushpacket(&vd, &op)) {
                ogg_stream_packetin(&os, &op);
                while (headerBytes ? ogg_stream_flush(&os, &og) : ogg_stream_pageout(&os, &og)) put(og);
            }
        }
    }
    while (ogg_stream_flush(&os, &og)) put(og);
    ogg_stream_clear(&os);
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    return out;
}

void setup(U32 object) {
    testNewInstruction(0);
    for (int s : { CS, DS, ES, SS }) cpu->seg[s].address = 0;
    memory->memset(HEAP, 0, 32 * K_PAGE_SIZE);
    U8 src[26] = { 0x71, 0x67, 2, 0, 0x44, 0xac, 0, 0, 0x80, 0x3e, 0, 0, 1, 0, 16, 0, 8, 0 }; // 0x6771, as the game's
    U8 dst[18] = { 1, 0, 2, 0, 0x44, 0xac, 0, 0, 0x10, 0xb1, 2, 0, 4, 0, 16, 0, 0, 0 };
    memory->memcpy(WFX_SRC, src, sizeof(src));
    memory->memcpy(WFX_DST, dst, sizeof(dst));
    memory->writed(INST, 40);
    memory->writed(INST + 4, WFX_SRC);
    memory->writed(INST + 8, WFX_DST);
    memory->writed(INST + 28, 2); // the driver's decode stream
    memory->writed(INST + 32, object);
}

// DriverProc(id, hdrv, msg, INST, HDR) as the guest's call arrives; true when done on the host, returning `result`.
bool driverProc(U32 msg, U32 result = 0) {
    ESP = STACK;
    memory->writed(STACK, RET);
    memory->writed(STACK + 4, 1);
    memory->writed(STACK + 8, 2);
    memory->writed(STACK + 12, msg);
    memory->writed(STACK + 16, INST);
    memory->writed(STACK + 20, HDR);
    EAX = 0xDEADBEEF;
    cpu->eip.u32 = 0x1000;
    bool ran = hostAcmRun(cpu);
    if (ran && (ESP != STACK + 24 || cpu->eip.u32 != RET || EAX != result)) testFail("convert did not return %u like `ret 0x14`", result);
    if (!ran && (ESP != STACK || cpu->eip.u32 != 0x1000 || EAX != 0xDEADBEEF)) testFail("declined call changed registers");
    return ran;
}

// One convert of src[0, len) with dstLen of room; used fields start at `used`, the PCM is appended to pcm.
bool convert(const U8* src, U32 len, U32 dstLen, std::vector<U8>& pcm, U32 used = 0, U32 flags = 0) {
    if (len) memory->memcpy(SRC, src, len);
    memory->writed(HDR + 12, SRC - used);
    memory->writed(HDR + 16, len + used);
    memory->writed(HDR + 20, used);
    memory->writed(HDR + 28, DST - used);
    memory->writed(HDR + 32, dstLen + used);
    memory->writed(HDR + 36, used);
    memory->writed(HDR + 44, flags);
    if (!driverProc(0x604f)) return false;
    if (memory->readd(HDR + 20) != len + used) testFail("source not all consumed: %d of %d", memory->readd(HDR + 20) - used, len);
    U32 out = memory->readd(HDR + 36) - used;
    if (out > dstLen || out % 4) testFail("bad cbDstLengthUsed %d (room %d)", out, dstLen);
    std::vector<U8> got(out);
    memory->memcpy(got.data(), DST, out);
    pcm.insert(pcm.end(), got.begin(), got.end());
    return true;
}

// Where each decoded block of the stream ends in the PCM, in bytes.
std::vector<U32> blockEnds(const std::vector<U8>& ogg) {
    ogg_sync_state oy;
    ogg_stream_state os;
    vorbis_info vi;
    vorbis_comment vc;
    vorbis_dsp_state vd;
    vorbis_block vb;
    ogg_page og;
    ogg_packet op;
    std::vector<U32> ends;
    ogg_sync_init(&oy);
    vorbis_info_init(&vi);
    vorbis_comment_init(&vc);
    ::memcpy(ogg_sync_buffer(&oy, (long)ogg.size()), ogg.data(), ogg.size());
    ogg_sync_wrote(&oy, (long)ogg.size());
    int headers = 0;
    bool streamInit = false;
    U32 total = 0;
    while (ogg_sync_pageout(&oy, &og) == 1) {
        if (!streamInit) ogg_stream_init(&os, ogg_page_serialno(&og));
        streamInit = true;
        ogg_stream_pagein(&os, &og);
        while (ogg_stream_packetout(&os, &op) == 1) {
            if (headers < 3) {
                vorbis_synthesis_headerin(&vi, &vc, &op);
                if (++headers == 3) {
                    vorbis_synthesis_init(&vd, &vi);
                    vorbis_block_init(&vd, &vb);
                }
                continue;
            }
            if (!vorbis_synthesis(&vb, &op)) vorbis_synthesis_blockin(&vd, &vb);
            int n = vorbis_synthesis_pcmout(&vd, nullptr);
            if (n <= 0) continue;
            vorbis_synthesis_read(&vd, n);
            total += n * 4;
            ends.push_back(total);
        }
    }
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    ogg_stream_clear(&os);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    ogg_sync_clear(&oy);
    return ends;
}

// A one-section PE whose .text says it holds `textSize` bytes, as hostAcmMapped sees the file.
bool mapFakeAcm(const char* path, U32 textSize, U32& driverProc) {
    std::vector<U8> pe(0x400);
    auto put32 = [&](U32 at, U32 v) { for (int i = 0; i < 4; i++) pe[at + i] = (U8)(v >> (i * 8)); };
    pe[0] = 'M'; pe[1] = 'Z';
    put32(0x3c, 0x40);
    ::memcpy(&pe[0x40], "PE\0\0", 4);
    pe[0x44] = 0x4c; pe[0x45] = 0x01; // i386
    pe[0x46] = 1; // one section
    pe[0x54] = 224; // optional header size
    U32 section = 0x40 + 24 + 224;
    ::memcpy(&pe[section], ".text\0\0", 8);
    put32(section + 16, textSize);
    put32(section + 20, 0x200);
    auto node = std::make_shared<FsMemNode>(1, 1, BString::copy(path));
    auto* open = new FsMemOpenNode(K_O_RDWR, node);
    open->writeNative(pe.data(), (U32)pe.size());
    open->seek(0);
    auto file = std::make_shared<KFile>(open);
    return hostAcmMapped(file.get(), driverProc);
}

}

// Catches: a corrupt vorbis.acm whose .text claims gigabytes making hostAcmParse's read allocate them first, which
// aborts the tab where Wine would just report a bad DLL (finding 85).
void testHostAcmBadFile() {
    U32 driverProc = 0;
    if (mapFakeAcm("/hostacm-test/vorbis.acm", 0xFFFFFFFF, driverProc)) testFail("a .text of 4 GB taken");
}

void testHostAcmConvert() {
    std::vector<U8> ogg = encode();
    std::vector<U8> whole, pieces, none;
    setup(0x1111);
    if (!convert(ogg.data(), (U32)ogg.size(), 0xE000, whole)) testFail("stream with headers not taken");
    if (whole.size() < 40000) testFail("decoded only %d bytes", (int)whole.size());
    driverProc(0x604d); // close: frees the host state (and runs the guest's close)
    setup(0x2222);
    // small source pieces (the first holds the headers, as the game's 64000-byte reads do) and a destination
    // smaller than a block, with used fields that don't start at 0
    U32 first = std::min((U32)ogg.size(), 6000u);
    if (!convert(ogg.data(), first, 3000, pieces, 8)) testFail("stream with headers not taken");
    for (U32 pos = first; pos < ogg.size(); pos += 700) convert(ogg.data() + pos, std::min(700u, (U32)ogg.size() - pos), 3000, pieces, 8);
    for (int i = 0; i < 40 && pieces.size() < whole.size(); i++) convert(nullptr, 0, 3000, pieces, 8);
    if (pieces != whole) testFail("PCM depends on the buffer split: %d vs %d bytes", (int)pieces.size(), (int)whole.size());
    driverProc(0x604d);
    // A destination that isn't a whole number of samples, or holds less than one: the convert must stop (it spun forever,
    // with the tab frozen, when a partly written block met 2 spare bytes) and no packet may be lost to the pending block.
    U32 headerBytes, next = 0x5000;
    std::vector<U8> paged = encode(&headerBytes), pagedWhole;
    setup(next++);
    if (!convert(paged.data(), (U32)paged.size(), 0xE000, pagedWhole) || pagedWhole.size() < 40000) testFail("page-per-packet stream not decoded");
    driverProc(0x604d);
    for (std::vector<U32> rooms : std::vector<std::vector<U32>>{ { 3002 }, { 3, 3000 }, { 0, 3000 }, { 3, 3, 3000 }, { 3001, 2, 3003 } }) {
        std::vector<U8> split;
        setup(next++);
        size_t room = 0;
        if (!convert(paged.data(), headerBytes, rooms[room++ % rooms.size()], split)) testFail("stream with headers not taken");
        for (U32 pos = headerBytes; pos < paged.size(); pos += 60) convert(paged.data() + pos, std::min(60u, (U32)paged.size() - pos), rooms[room++ % rooms.size()], split);
        for (int i = 0; i < 400 && split.size() < pagedWhole.size(); i++) convert(nullptr, 0, rooms[room++ % rooms.size()], split);
        if (split != pagedWhole) testFail("PCM depends on the room left (first room %u): %d vs %d bytes", rooms[0], (int)split.size(), (int)pagedWhole.size());
        driverProc(0x604d);
    }
    // A debug trap (hardware breakpoint set, or TF) on a stream the host owns: the guest decoder saw none of its pages, so
    // it stays on the host, and its close still frees the host state (the same object then starts from nothing).
    std::vector<U8> trapped;
    setup(0x6000);
    if (!convert(paged.data(), headerBytes, 3000, trapped)) testFail("stream with headers not taken");
    cpu->debugTrapActive = true;
    for (U32 pos = headerBytes; pos < paged.size(); pos += 60) {
        if (!convert(paged.data() + pos, std::min(60u, (U32)paged.size() - pos), 3000, trapped)) testFail("owned stream left to the guest under a debug trap");
    }
    for (int i = 0; i < 40 && trapped.size() < pagedWhole.size(); i++) convert(nullptr, 0, 3000, trapped);
    if (trapped != pagedWhole) testFail("PCM differs with a debug trap on: %d vs %d bytes", (int)trapped.size(), (int)pagedWhole.size());
    driverProc(0x604d);
    setup(0x6000);
    trapped.clear();
    if (!convert(paged.data(), (U32)paged.size(), 0xE000, trapped) || trapped != pagedWhole) testFail("close under a debug trap did not free the host stream");
    driverProc(0x604d);
    // A first convert under a debug trap leaves the stream to the guest for good: its decoder has then seen the headers.
    setup(0x6100);
    cpu->debugTrapActive = true;
    if (convert(ogg.data(), (U32)ogg.size(), 0xE000, none)) testFail("new stream taken under a debug trap");
    cpu->debugTrapActive = false;
    if (convert(ogg.data(), (U32)ogg.size(), 0xE000, none)) testFail("stream first seen under a debug trap taken later");
    driverProc(0x604d);
    // a stream whose first convert has no headers stays with the guest, even when headers come later
    setup(0x3333);
    if (convert(ogg.data() + ogg.size() / 2, (U32)(ogg.size() - ogg.size() / 2), 0xE000, none)) testFail("headerless stream taken");
    if (convert(ogg.data(), (U32)ogg.size(), 0xE000, none)) testFail("guest stream taken later");
    driverProc(0x604d);
    // The destination fills exactly when a block ends, with more packets of the file's last page still queued. Catches
    // them waiting for a page that never comes: the next call returned nothing, and END then dropped them for good, so
    // BGM or a voice line lost its tail (finding 87). The whole stream sits in one page here, so every end is such a case.
    std::vector<U32> ends = blockEnds(ogg);
    if (ends.size() < 4 || ends.back() != whole.size()) testFail("block ends don't add up: %d blocks, %d bytes", (int)ends.size(), ends.empty() ? 0 : (int)ends.back());
    next = 0x7000;
    for (size_t e = 0; e + 1 < ends.size(); e++) {
        for (U32 flags : { 0u, ACM_END }) {
            for (U32 lastFlags : { 0u, ACM_END }) {
                std::vector<U8> cut;
                setup(next++);
                if (!convert(ogg.data(), (U32)ogg.size(), ends[e], cut, 0, flags)) testFail("stream with headers not taken");
                if (cut.size() != ends[e]) testFail("exact fill gave %d bytes of %u", (int)cut.size(), ends[e]);
                convert(nullptr, 0, 0xE000, cut, 0, lastFlags);
                if (cut != whole) testFail("tail lost after an exact fill at %u (flags %x, %x): %d vs %d bytes", ends[e], flags, lastFlags, (int)cut.size(), (int)whole.size());
                driverProc(0x604d);
            }
        }
    }
    // A stream ends once: after everything came out under END, more source gives nothing.
    std::vector<U8> ended;
    setup(0x6200);
    if (!convert(ogg.data(), (U32)ogg.size(), 0xE000, ended, 0, ACM_END) || ended != whole) testFail("stream under END not whole");
    convert(ogg.data(), (U32)ogg.size(), 0xE000, ended);
    if (ended != whole) testFail("PCM after the end of a stream: %d bytes", (int)(ended.size() - whole.size()));
    driverProc(0x604d);
    // A bad cbSrcLength: nothing maps those 3 GB, and the buffer may not be copied before that is known. Catches the copy
    // aborting the tab (with memory that can't grow that far) where the guest gets an error or a fault it can handle (finding 85).
    auto badSource = [](U32 result) {
        memory->writed(HDR + 12, 0x10000000);
        memory->writed(HDR + 16, 0xC0000000);
        memory->writed(HDR + 20, 0);
        memory->writed(HDR + 28, DST);
        memory->writed(HDR + 32, 3000);
        memory->writed(HDR + 36, 0);
        return driverProc(0x604f, result);
    };
    setup(0x6300);
    if (badSource(0)) testFail("first convert with a bad source taken");
    if (memory->readd(HDR + 20) || memory->readd(HDR + 36)) testFail("declined convert wrote its used fields");
    driverProc(0x604d);
    setup(0x6400);
    if (!convert(ogg.data(), first, 3000, none)) testFail("stream with headers not taken");
    if (!badSource(ACM_INVALPARAM)) testFail("owned stream with a bad source left to the guest");
    if (memory->readd(HDR + 20) || memory->readd(HDR + 36)) testFail("failed convert wrote its used fields");
    driverProc(0x604d);
}

#endif
