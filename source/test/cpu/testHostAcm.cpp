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

bool hostAcmRun(CPU* cpu);

#define cpu (testContext().cpu)
#define memory (testContext().memory)

namespace {

constexpr U32 HEAP = 0xF0000000; // 32 RW pages (testCPU.cpp)
constexpr U32 STACK = HEAP + 0x400, INST = HEAP + 0x500, HDR = HEAP + 0x600, WFX_SRC = HEAP + 0x700, WFX_DST = HEAP + 0x740;
constexpr U32 SRC = HEAP + 0x1000, DST = HEAP + 0x10000, RET = 0x12345678;

// 0.25 s of a stereo 44.1 kHz tone as an Ogg Vorbis stream (headers on their own pages, as encoders write them).
std::vector<U8> encode() {
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
                while (ogg_stream_pageout(&os, &og)) put(og);
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

// DriverProc(id, hdrv, msg, INST, HDR) as the guest's call arrives; true when done on the host.
bool driverProc(U32 msg) {
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
    if (ran && (ESP != STACK + 24 || cpu->eip.u32 != RET || EAX)) testFail("convert did not return 0 like `ret 0x14`");
    if (!ran && (ESP != STACK || cpu->eip.u32 != 0x1000 || EAX != 0xDEADBEEF)) testFail("declined call changed registers");
    return ran;
}

// One convert of src[0, len) with dstLen of room; used fields start at `used`, the PCM is appended to pcm.
bool convert(const U8* src, U32 len, U32 dstLen, std::vector<U8>& pcm, U32 used = 0) {
    if (len) memory->memcpy(SRC, src, len);
    memory->writed(HDR + 12, SRC - used);
    memory->writed(HDR + 16, len + used);
    memory->writed(HDR + 20, used);
    memory->writed(HDR + 28, DST - used);
    memory->writed(HDR + 32, dstLen + used);
    memory->writed(HDR + 36, used);
    memory->writed(HDR + 44, 0);
    if (!driverProc(0x604f)) return false;
    if (memory->readd(HDR + 20) != len + used) testFail("source not all consumed: %d of %d", memory->readd(HDR + 20) - used, len);
    U32 out = memory->readd(HDR + 36) - used;
    if (out > dstLen || out % 4) testFail("bad cbDstLengthUsed %d (room %d)", out, dstLen);
    std::vector<U8> got(out);
    memory->memcpy(got.data(), DST, out);
    pcm.insert(pcm.end(), got.begin(), got.end());
    return true;
}

}

void testHostAcmConvert() {
    std::vector<U8> ogg = encode();
    std::vector<U8> whole, pieces;
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
    // a stream whose first convert has no headers stays with the guest, even when headers come later
    setup(0x3333);
    std::vector<U8> none;
    if (convert(ogg.data() + ogg.size() / 2, (U32)(ogg.size() - ogg.size() / 2), 0xE000, none)) testFail("headerless stream taken");
    if (convert(ogg.data(), (U32)ogg.size(), 0xE000, none)) testFail("guest stream taken later");
    driverProc(0x604d);
}

#endif
