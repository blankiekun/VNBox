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
U32 hostVorbisFileLoop(KMemory* memory, U32 ovRead);

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
    // decodes every packet the way vorbisfile does; a debug trap (hardware breakpoint set, or TF) is on from packet trapAt
    auto decode = [&](size_t trapAt, std::vector<float>& got) {
    for (size_t i = 3; i < packets.size(); i++) {
        cpu->debugTrapActive = i >= trapAt;
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
    cpu->debugTrapActive = false;
    };
    guestInit(blocksize1, VI2);
    initCalls(packets, VI2); // headers went into VI, the stream decodes with its copy
    decode(packets.size(), got);
    if (got.size() < 20000) testFail("decoded only %d samples", (int)got.size() / 2);
    if (got != want) testFail("samples differ from host libvorbis: %d vs %d", (int)got.size(), (int)want.size());
    clearCalls();
    // A debug trap on a stream the host already decodes: the guest's state was never advanced, so it stays on the host.
    std::vector<float> trapped;
    guestInit(blocksize1, VI2);
    initCalls(packets, VI2);
    decode(4, trapped);
    if (trapped != want) testFail("samples differ with a debug trap on: %d vs %d", (int)trapped.size(), (int)want.size());
    cpu->debugTrapActive = true;
    clearCalls(); // the host copies must go even under the trap
    cpu->debugTrapActive = false;
    putPacket(packets[3]);
    if (call("vorbis_synthesis", { VB, OP })) testFail("objects cleared under a debug trap still taken");
    // Mirrored calls have no guest effect of their own, so they follow under a trap: nothing has diverged yet at the first host-only call.
    trapped.clear();
    guestInit(blocksize1, VI2);
    cpu->debugTrapActive = true;
    initCalls(packets, VI2);
    cpu->debugTrapActive = false;
    decode(packets.size(), trapped);
    if (trapped != want) testFail("samples differ after init under a debug trap: %d vs %d", (int)trapped.size(), (int)want.size());
    clearCalls();
    // That first host-only call is where the stream is decided: under a trap it stays guest code for good.
    guestInit(blocksize1, VI2);
    initCalls(packets, VI2);
    putPacket(packets[3]);
    cpu->debugTrapActive = true;
    if (call("vorbis_synthesis", { VB, OP })) testFail("new stream taken under a debug trap");
    cpu->debugTrapActive = false;
    if (call("vorbis_synthesis", { VB, OP }) || call("vorbis_synthesis_pcmout", { VD, 0 })) testFail("stream decided under a debug trap taken later");
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

// What a seek does (ov_raw_seek, ov_pcm_seek, so a BGM loop that seeks): vorbis_synthesis_restart mid-stream, packets from
// elsewhere, some of them only tracked (trackonly + blockin, samples counted, not read), then decoding again. In lock-step
// with host libvorbis making the same calls: same samples and the same public fields vorbisfile reads. Catches the restart
// or trackonly not reaching the host copy, which would click or shift at every loop. Then a second stream on the same guest
// objects with no clear between (a new id header): catches a block or dsp state kept from the first stream.
void testHostVorbisSeek() {
    std::vector<std::vector<U8>> store;
    int blocksize1;
    std::vector<ogg_packet> packets = encode(store, blocksize1);
    vorbis_info vi;
    vorbis_comment vc;
    vorbis_dsp_state vd;
    vorbis_block vb;
    auto hostInit = [&]() {
        vorbis_info_init(&vi);
        vorbis_comment_init(&vc);
        for (int i = 0; i < 3; i++) vorbis_synthesis_headerin(&vi, &vc, &packets[i]);
        vorbis_synthesis_init(&vd, &vi);
        vorbis_block_init(&vd, &vb);
    };
    auto hostClear = [&]() {
        vorbis_block_clear(&vb);
        vorbis_dsp_clear(&vd);
        vorbis_comment_clear(&vc);
        vorbis_info_clear(&vi);
    };
    U32 samples = 0;
    auto step = [&](size_t i, bool track) {
        ogg_packet& p = packets[i];
        putPacket(p);
        int want = track ? vorbis_synthesis_trackonly(&vb, &p) : vorbis_synthesis(&vb, &p);
        if (!call(track ? "vorbis_synthesis_trackonly" : "vorbis_synthesis", { VB, OP }) || (int)EAX != want || want) testFail("packet %d: result %d, not %d", (int)i, (int)EAX, want);
        want = vorbis_synthesis_blockin(&vd, &vb);
        if (!call("vorbis_synthesis_blockin", { VD, VB }) || (int)EAX != want) testFail("packet %d: blockin", (int)i);
        if (memory->readd(VD + 0x14) != (U32)vd.pcm_current || memory->readd(VD + 0x18) != (U32)vd.pcm_returned || memory->readd(VD + 0x20) != (U32)vd.eofflag ||
            (S64)memory->readq(VD + 0x38) != (S64)vd.granulepos || (S64)memory->readq(VD + 0x40) != (S64)vd.sequence) testFail("packet %d: guest vd's public fields out of step", (int)i);
        float** pcm;
        int n = vorbis_synthesis_pcmout(&vd, track ? nullptr : &pcm);
        if (!call("vorbis_synthesis_pcmout", { VD, track ? 0 : PCMOUT }) || (int)EAX != n) testFail("packet %d: %d samples, not %d", (int)i, (int)EAX, n);
        if (n > 0 && !track) {
            for (int ch = 0; ch < 2; ch++) {
                if (memory->memcmp(memory->readd(RETARR + ch * 4), pcm[ch], n * 4)) testFail("packet %d: samples of channel %d differ", (int)i, ch);
            }
        }
        samples += n > 0 ? n : 0;
        vorbis_synthesis_read(&vd, n);
        if (n > 0 && (!call("vorbis_synthesis_read", { VD, (U32)n }) || EAX)) testFail("packet %d: read", (int)i);
    };
    size_t mid = 3 + (packets.size() - 3) / 3;
    if (packets.size() < mid + 8) testFail("too few packets: %d", (int)packets.size());
    guestInit(blocksize1, VI2);
    initCalls(packets, VI2);
    hostInit();
    for (size_t i = 3; i < mid; i++) step(i, false);
    if (call("vorbis_synthesis_restart", { VD })) testFail("restart not left to the guest too");
    vorbis_synthesis_restart(&vd);
    for (size_t i = mid; i < mid + 2; i++) step(i, true);
    samples = 0;
    for (size_t i = mid + 2; i < packets.size(); i++) step(i, false);
    if (samples < 1000) testFail("only %u samples after the seek", samples);
    hostClear();
    // the next stream's headers on the same objects, no clear
    initCalls(packets, VI2);
    hostInit();
    samples = 0;
    for (size_t i = 3; i < packets.size(); i++) step(i, false);
    if (samples < 10000) testFail("only %u samples of the second stream", samples);
    hostClear();
    clearCalls();
}

// Native libvorbisfile (hostvorbis.cpp): ov_read_filter's 16-bit pack loop, as 最果てのイマ's libvorbisfile.dll has it
// (with ov_read and the two helpers it calls), run as guest code by the emulator and hooked on the host, must leave the
// same samples and locals, in every x87 rounding mode, for decoded samples and for edge values (ties, clipping, NaN,
// infinities, out of int32 range). ov_read is found by its bytes; any other bytes, or another constant, stay guest.
void testHostVorbisPack16() {
    static const U8 ovRead[] = {
        0x55, 0x8b, 0xec, 0x6a, 0x00, 0x6a, 0x00, 0x8b, 0x45, 0x20, 0x50, 0x8b, 0x4d, 0x1c, 0x51, 0x8b, 0x55, 0x18, 0x52, 0x8b,
        0x45, 0x14, 0x50, 0x8b, 0x4d, 0x10, 0x51, 0x8b, 0x55, 0x0c, 0x52, 0x8b, 0x45, 0x08, 0x50, 0xe8, 0xc8, 0xf9, 0xff, 0xff,
        0x83, 0xc4, 0x24, 0x5d, 0xc3 };
    static const U8 loop[] = { // RVA 0x4f16..0x4fd1; the fmul's address at +0x67 is 0x10015228 (32768.0) unrelocated
        0x8d, 0x55, 0xe0, 0x52, 0xe8, 0xd1, 0x03, 0x00, 0x00, 0x83, 0xc4, 0x04, 0xc7, 0x45, 0xf8, 0x00, 0x00, 0x00, 0x00, 0xeb,
        0x09, 0x8b, 0x45, 0xf8, 0x83, 0xc0, 0x01, 0x89, 0x45, 0xf8, 0x8b, 0x4d, 0xf8, 0x3b, 0x4d, 0xdc, 0x0f, 0x8d, 0x91, 0x00,
        0x00, 0x00, 0x8b, 0x55, 0xf8, 0x8b, 0x45, 0xfc, 0x8b, 0x0c, 0x90, 0x89, 0x4d, 0xc8, 0x8b, 0x55, 0xf8, 0x8b, 0x45, 0x0c,
        0x8d, 0x0c, 0x50, 0x89, 0x4d, 0xcc, 0xc7, 0x45, 0xec, 0x00, 0x00, 0x00, 0x00, 0xeb, 0x09, 0x8b, 0x55, 0xec, 0x83, 0xc2,
        0x01, 0x89, 0x55, 0xec, 0x8b, 0x45, 0xec, 0x3b, 0x45, 0xf4, 0x7d, 0x5a, 0x8b, 0x4d, 0xec, 0x8b, 0x55, 0xc8, 0xd9, 0x04,
        0x8a, 0xdc, 0x0d, 0x28, 0x52, 0x01, 0x10, 0x83, 0xec, 0x08, 0xdd, 0x1c, 0x24, 0xe8, 0x44, 0x03, 0x00, 0x00, 0x83, 0xc4,
        0x08, 0x89, 0x45, 0xd8, 0x81, 0x7d, 0xd8, 0xff, 0x7f, 0x00, 0x00, 0x7e, 0x09, 0xc7, 0x45, 0xd8, 0xff, 0x7f, 0x00, 0x00,
        0xeb, 0x10, 0x81, 0x7d, 0xd8, 0x00, 0x80, 0xff, 0xff, 0x7d, 0x07, 0xc7, 0x45, 0xd8, 0x00, 0x80, 0xff, 0xff, 0x8b, 0x45,
        0xcc, 0x66, 0x8b, 0x4d, 0xd8, 0x66, 0x89, 0x08, 0x8b, 0x55, 0xdc, 0x8b, 0x45, 0xcc, 0x8d, 0x0c, 0x50, 0x89, 0x4d, 0xcc,
        0xeb, 0x95, 0xe9, 0x5a, 0xff, 0xff, 0xff };
    static const U8 ftoi[] = { 0x55, 0x8b, 0xec, 0x51, 0xdd, 0x45, 0x08, 0xdb, 0x5d, 0xfc, 0x8b, 0x45, 0xfc, 0x8b, 0xe5, 0x5d, 0xc3 };
    static const U8 setround[] = { 0x55, 0x8b, 0xec, 0x5d, 0xc3 };
    constexpr U32 LOOP = TEST_CODE_ADDRESS + 0x100, EXIT = LOOP + sizeof(loop), OVREAD = LOOP + 0x42a, SCALE = HEAP + 0x40;
    constexpr U32 PCMS = HEAP + 0x80, SRC = HEAP + 0x1000, EBPV = TEST_STACK_ADDRESS - 0x800;
    constexpr U32 MAXN = 6000, OUTB = SRC + 2 * MAXN * 4;
    U32 pack = hostVorbisFnId("ov_read 16-bit pack loop");
    if (!pack) testFail("no pack loop id");

    std::vector<std::vector<U8>> store;
    int blocksize1;
    std::vector<float> decoded = reference(encode(store, blocksize1)); // interleaved stereo
    float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
    const float edge[] = { 0.5f / 32768, 1.5f / 32768, 2.5f / 32768, -0.5f / 32768, -1.5f / 32768, -2.5f / 32768,
        32766.5f / 32768, 32767.5f / 32768, -32767.5f / 32768, -32768.5f / 32768, 1.0f, -1.0f, 1.5f, -1.5f, 65535.99f,
        65536.0f, 65537.0f, -65536.0f, -65537.0f, 1e10f, -1e10f, inf, -inf, nan, -nan, 1e-40f, -1e-40f, 0.0f, -0.0f };

    // channels, samples, the guest's x87 rounding control: memory after the loop (output, then the frame's locals)
    auto run = [&](S32 channels, S32 samples, U32 rc, bool host, double scale) -> std::vector<U8> {
        testNewInstruction(0);
        for (int seg : { CS, DS, ES, SS, FS, GS }) cpu->seg[seg].address = 0;
        memory->memcpy(LOOP, loop, sizeof(loop));
        memory->writed(LOOP + 0x67, SCALE);
        memory->memcpy(LOOP + 0x3ba, ftoi, sizeof(ftoi));
        memory->memcpy(LOOP + 0x3da, setround, sizeof(setround));
        memory->memcpy(OVREAD, ovRead, sizeof(ovRead));
        for (U32 page = LOOP >> K_PAGE_SHIFT; page <= (OVREAD + 0x40) >> K_PAGE_SHIFT; page++) memory->clearPageWriteCounts(page);
        U64 bits;
        ::memcpy(&bits, &scale, 8);
        memory->writeq(SCALE, bits);
        for (S32 ch = 0; ch < 2; ch++) {
            memory->writed(PCMS + ch * 4, SRC + ch * MAXN * 4);
            for (U32 j = 0; j < MAXN; j++) {
                float f = j < sizeof(edge) / 4 ? (ch ? -edge[j] : edge[j]) : decoded[((j * 2) % decoded.size()) + ch];
                U32 v;
                ::memcpy(&v, &f, 4);
                memory->writed(SRC + (ch * MAXN + j) * 4, v);
            }
        }
        memory->memset(OUTB, 0xaa, 2 * MAXN * 2);
        memory->memset(EBPV - 0x40, 0x55, 0x50);
        memory->writed(EBPV - 4, PCMS);
        memory->writed(EBPV - 0xc, (U32)samples);
        memory->writed(EBPV - 0x24, (U32)channels);
        memory->writed(EBPV + 0xc, OUTB);
        cpu->fpu.SetCW((U16)(0x037f | (rc << 10)));
        EBP = EBPV;
        ESP = EBPV - 0x44;
        cpu->eip.u32 = LOOP;
        if (host) {
            if (!hostVorbisRun(cpu, pack - 1)) return {};
            if (cpu->eip.u32 != EXIT || ESP != EBPV - 0x44) testFail("pack loop didn't end at its exit");
        } else {
            testContext().codeIp = EXIT; // TestEnd at the loop's exit
            testRunCPU();
        }
        std::vector<U8> got(2 * MAXN * 2 + 0x50);
        memory->memcpy(got.data(), OUTB, 2 * MAXN * 2);
        memory->memcpy(got.data() + 2 * MAXN * 2, EBPV - 0x40, 0x50);
        return got;
    };
    struct Case { S32 channels, samples; };
    for (Case c : { Case{ 2, (S32)MAXN }, Case{ 1, 777 }, Case{ 2, 0 }, Case{ 0, 50 }, Case{ 2, -3 } }) {
        for (U32 rc = 0; rc < 4; rc++) {
            std::vector<U8> guest = run(c.channels, c.samples, rc, false, 32768.0), host = run(c.channels, c.samples, rc, true, 32768.0);
            if (host.empty()) testFail("pack loop (%d ch, %d samples) not done on the host", c.channels, c.samples);
            if (guest != host) {
                size_t k = 0;
                while (guest[k] == host[k]) k++;
                testFail("pack loop (%d ch, %d samples, rc %d) differs from the guest at byte %d", c.channels, c.samples, rc, (int)k);
            }
        }
    }
    if (!run(2, 100, 0, true, 32767.0).empty()) testFail("pack loop with another constant taken");
    // found from ov_read by its bytes, and only then
    run(2, 1, 0, true, 32768.0);
    if (hostVorbisFileLoop(memory, OVREAD) != LOOP) testFail("known libvorbisfile not recognized");
    memory->writeb(LOOP + 0x7f, 0xfe); // the clamp at 32767
    if (hostVorbisFileLoop(memory, OVREAD)) testFail("changed pack loop recognized");
    memory->writeb(LOOP + 0x7f, 0xff);
    memory->writeb(OVREAD + 0x2c, 0xc2);
    if (hostVorbisFileLoop(memory, OVREAD)) testFail("changed ov_read recognized");
}

#endif
