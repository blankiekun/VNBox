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

// Native libvorbis (hostvorbis.cpp): the call sequence vorbisfile makes (headerin x3, synthesis_init, block_init, then
// synthesis/blockin/pcmout/read per packet) on guest objects must give the same samples as host libvorbis decoding the
// stream directly, write them into the guest's own pcm buffers, keep the guest vd's public fields in step, return like
// a cdecl `ret`, and leave the mirrored calls to the guest. A vd whose layout doesn't match stays guest code.
#include <ogg/ogg.h>
#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>
#include "testHostVorbis.h"
#include "testCPU.h"

U32 hostVorbisFnId(const char* name);
bool hostVorbisRun(CPU* cpu, U32 fn);

#define cpu (testContext().cpu)
#define memory (testContext().memory)

namespace {

constexpr U32 HEAP = 0xF0000000; // 32 RW pages (testCPU.cpp)
constexpr U32 STACK = HEAP + 0x400, VI = HEAP + 0x500, VI2 = HEAP + 0x5a0, SETUP = HEAP + 0x5e0, VC = HEAP + 0x540, VD = HEAP + 0x600, VB = HEAP + 0x700;
constexpr U32 OP = HEAP + 0x800, PCMARR = HEAP + 0x900, RETARR = HEAP + 0x910, PCMOUT = HEAP + 0x920;
constexpr U32 PACKET = HEAP + 0x1000, CH0 = HEAP + 0x8000, CH1 = HEAP + 0x10000, RET = 0x12345678;

// The packets of 0.25 s of a stereo 44.1 kHz tone.
std::vector<ogg_packet> encode(std::vector<std::vector<U8>>& store, int& blocksize1) {
    vorbis_info vi;
    vorbis_comment vc;
    vorbis_dsp_state vd;
    vorbis_block vb;
    ogg_packet op, h[3];
    std::vector<ogg_packet> out;
    auto keep = [&](const ogg_packet& p) {
        store.emplace_back(p.packet, p.packet + p.bytes);
        out.push_back(p);
    };
    vorbis_info_init(&vi);
    vorbis_encode_init_vbr(&vi, 2, 44100, 0.1f);
    blocksize1 = vorbis_info_blocksize(&vi, 1);
    vorbis_comment_init(&vc);
    vorbis_analysis_init(&vd, &vi);
    vorbis_block_init(&vd, &vb);
    vorbis_analysis_headerout(&vd, &vc, &h[0], &h[1], &h[2]);
    for (auto& p : h) keep(p);
    for (int done = 0, n = 1; n; done += n) {
        n = std::min(1024, 11025 - done);
        if (n) {
            float** buf = vorbis_analysis_buffer(&vd, n);
            for (int i = 0; i < n; i++) buf[0][i] = buf[1][i] = 0.5f * sinf((done + i) * 0.0627f);
        }
        vorbis_analysis_wrote(&vd, n);
        while (vorbis_analysis_blockout(&vd, &vb) == 1) {
            vorbis_analysis(&vb, nullptr);
            vorbis_bitrate_addblock(&vb);
            while (vorbis_bitrate_flushpacket(&vd, &op)) keep(op);
        }
    }
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    for (size_t i = 0; i < out.size(); i++) out[i].packet = store[i].data(); // store no longer moves
    return out;
}

// Reference: host libvorbis on the packets, all samples per channel.
std::vector<float> reference(const std::vector<ogg_packet>& packets) {
    vorbis_info vi;
    vorbis_comment vc;
    vorbis_dsp_state vd;
    vorbis_block vb;
    vorbis_info_init(&vi);
    vorbis_comment_init(&vc);
    for (int i = 0; i < 3; i++) vorbis_synthesis_headerin(&vi, &vc, const_cast<ogg_packet*>(&packets[i]));
    vorbis_synthesis_init(&vd, &vi);
    vorbis_block_init(&vd, &vb);
    std::vector<float> out;
    for (size_t i = 3; i < packets.size(); i++) {
        if (!vorbis_synthesis(&vb, const_cast<ogg_packet*>(&packets[i]))) vorbis_synthesis_blockin(&vd, &vb);
        float** pcm;
        int n;
        while ((n = vorbis_synthesis_pcmout(&vd, &pcm)) > 0) {
            for (int j = 0; j < n; j++) out.insert(out.end(), { pcm[0][j], pcm[1][j] });
            vorbis_synthesis_read(&vd, n);
        }
    }
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    return out;
}

// fn(args) as the guest's cdecl call arrives; true when done on the host (then EAX is the result).
bool call(const char* name, std::initializer_list<U32> args) {
    U32 fn = hostVorbisFnId(name);
    if (!fn) testFail("%s not hooked", name);
    ESP = STACK;
    memory->writed(STACK, RET);
    U32 a = STACK + 4;
    for (U32 v : args) { memory->writed(a, v); a += 4; }
    EAX = 0xDEADBEEF;
    cpu->eip.u32 = 0x1000;
    bool ran = hostVorbisRun(cpu, fn - 1);
    if (ran && (ESP != STACK + 4 || cpu->eip.u32 != RET)) testFail("%s did not return like `ret`", name);
    if (!ran && (ESP != STACK || cpu->eip.u32 != 0x1000 || EAX != 0xDEADBEEF)) testFail("declined %s changed registers", name);
    return ran;
}

void putPacket(const ogg_packet& p) {
    memory->memcpy(PACKET, p.packet, (U32)p.bytes);
    memory->writed(OP, PACKET);
    memory->writed(OP + 4, (U32)p.bytes);
    memory->writed(OP + 8, (U32)p.b_o_s);
    memory->writed(OP + 12, (U32)p.e_o_s);
    memory->writeq(OP + 16, (U64)p.granulepos);
    memory->writeq(OP + 24, (U64)p.packetno);
}

// What the guest's vorbis_info_init + headerin x3 + vorbis_synthesis_init leave in the guest objects that matter here.
void guestInit(int storage, U32 vdVi) {
    testNewInstruction(0);
    for (int s : { CS, DS, ES, SS }) cpu->seg[s].address = 0;
    memory->memset(HEAP, 0, 32 * K_PAGE_SIZE);
    for (U32 vi : { VI, VI2 }) { // VI2: a by-value copy, as vorbisfile makes (same codec_setup)
        memory->writed(vi + 4, 2); // channels
        memory->writed(vi + 8, 44100);
        memory->writed(vi + 0x1c, SETUP);
    }
    memory->writed(VD + 4, vdVi);
    memory->writed(VD + 8, PCMARR);
    memory->writed(VD + 0xc, RETARR);
    memory->writed(VD + 0x10, (U32)storage);
    memory->writed(PCMARR, CH0);
    memory->writed(PCMARR + 4, CH1);
}

void initCalls(const std::vector<ogg_packet>& packets, U32 initVi) {
    for (int i = 0; i < 3; i++) {
        putPacket(packets[i]);
        if (call("vorbis_synthesis_headerin", { VI, VC, OP })) testFail("headerin not left to the guest");
    }
    if (call("vorbis_synthesis_init", { VD, initVi })) testFail("synthesis_init not left to the guest");
    if (call("vorbis_block_init", { VD, VB })) testFail("block_init not left to the guest");
}

void clearCalls() {
    call("vorbis_block_clear", { VB });
    call("vorbis_dsp_clear", { VD });
    call("vorbis_info_clear", { VI });
}

}

void testHostVorbisDecode() {
    std::vector<std::vector<U8>> store;
    int blocksize1;
    std::vector<ogg_packet> packets = encode(store, blocksize1);
    std::vector<float> want = reference(packets), got;
    guestInit(blocksize1, VI2);
    initCalls(packets, VI2); // headers went into VI, the stream decodes with its copy
    for (size_t i = 3; i < packets.size(); i++) {
        putPacket(packets[i]);
        if (!call("vorbis_synthesis", { VB, OP })) testFail("synthesis not done on the host");
        if (!EAX && (!call("vorbis_synthesis_blockin", { VD, VB }) || EAX)) testFail("blockin failed");
        for (;;) {
            if (!call("vorbis_synthesis_pcmout", { VD, 0 })) testFail("pcmout not done on the host");
            U32 avail = EAX;
            if (!avail) break;
            U32 returned = memory->readd(VD + 0x18);
            if (memory->readd(VD + 0x14) - returned != avail) testFail("guest pcm_current/pcm_returned out of step");
            call("vorbis_synthesis_pcmout", { VD, PCMOUT });
            if (EAX != avail || memory->readd(PCMOUT) != RETARR) testFail("pcmout with pcm: bad result");
            U32 c0 = memory->readd(RETARR), c1 = memory->readd(RETARR + 4);
            if (c0 != CH0 + returned * 4 || c1 != CH1 + returned * 4) testFail("pcmret doesn't point into the guest's pcm buffers");
            U32 n = std::min(avail, 300u); // vorbisfile may take less than a block
            for (U32 j = 0; j < n; j++) {
                U32 l = memory->readd(c0 + j * 4), r = memory->readd(c1 + j * 4);
                float fl, fr;
                ::memcpy(&fl, &l, 4);
                ::memcpy(&fr, &r, 4);
                got.insert(got.end(), { fl, fr });
            }
            if (!call("vorbis_synthesis_read", { VD, n }) || EAX) testFail("read failed");
        }
    }
    if (got.size() < 20000) testFail("decoded only %d samples", (int)got.size() / 2);
    if (got != want) testFail("samples differ from host libvorbis: %d vs %d", (int)got.size(), (int)want.size());
    clearCalls();
    // a guest vd that doesn't point at the vi it was initialized with (another layout): left to the guest for good
    guestInit(blocksize1, VI + 4);
    initCalls(packets, VI);
    putPacket(packets[3]);
    if (call("vorbis_synthesis", { VB, OP })) testFail("mismatched vd taken");
    if (call("vorbis_synthesis_pcmout", { VD, 0 })) testFail("mismatched vd's pcmout taken");
    clearCalls();
    // objects never seen through init (the clears ran, or headers came another way) stay guest code
    putPacket(packets[3]);
    if (call("vorbis_synthesis", { VB, OP }) || call("vorbis_synthesis_read", { VD, 1 })) testFail("unknown objects taken");
}

#endif
