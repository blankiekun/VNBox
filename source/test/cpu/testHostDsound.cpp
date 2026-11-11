/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "boxedwine.h"

#ifdef __TEST

// Host dsound mixing (hostdsound.cpp): vnbox_dsound_convert and vnbox_dsound_mix run as host code must leave guest memory
// exactly as the guest functions do. The reference is the guest code itself, run by the emulator: the two functions'
// bytes from jp.zip's dsound.dll.so (vn/build-wine-boot.sh with vn/wine/dsound-mixer-fast.patch, Ubuntu 24.04's
// GCC 13.3, -O2 -msse2 -march=pentium4 -mfpmath=sse), with their three absolute references to the 1/32768 constant
// moved to a copy of it. Catches: a different rounding or operand order (volume then mix, the s16 scale), a byte
// between the frames' values written, the dup copy, the volume not written back to src, and a call it should leave
// to the guest taken anyway (overlap, a read-only page, arguments the patched dsound never passes).
#include "testHostDsound.h"
#include "testCPU.h"

bool hostDsoundRun(CPU* cpu, U32 fn);

#define cpu (testContext().cpu)
#define memory (testContext().memory)

namespace {

constexpr U32 DS_CONVERT = 0, DS_MIX = 1;
constexpr U32 HEAP = TEST_HEAP_ADDRESS; // 32 RW pages (testCPU.cpp)
constexpr U32 IN = HEAP, OUT = HEAP + 0x2000, SRC = HEAP + 0x4000, DST = HEAP + 0x6000, VOLS = HEAP + 0x8000;
constexpr U32 DATA_END = HEAP + 0x8200, SCALE = DATA_END, STACK = HEAP + 0x10000, RET = 0x12345678;
constexpr U32 CONVERT_FN = TEST_CODE_ADDRESS + 0x1000, MIX_FN = TEST_CODE_ADDRESS + 0x1200;

const U8 convertCode[] = { // vnbox_dsound_convert, 0x274c9 in that dsound.dll.so
    0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x8b, 0x55, 0x08, 0x8b, 0x7d, 0x0c, 0x8b, 0x45, 0x10, 0x8b,
    0x5d, 0x14, 0x8b, 0x75, 0x1c, 0x8b, 0x4d, 0x20, 0x85, 0xc9, 0x0f, 0x95, 0xc1, 0x83, 0xfe, 0x02,
    0x75, 0x51, 0x84, 0xc9, 0x74, 0x4d, 0x8b, 0x4d, 0x18, 0x85, 0xc9, 0x74, 0x41, 0x8d, 0x0c, 0x1b,
    0x01, 0xc9, 0xf3, 0x0f, 0x10, 0x0d, 0x0c, 0x78, 0x03, 0x00, 0x8b, 0x5d, 0x18, 0x0f, 0xbf, 0x32,
    0x66, 0x0f, 0xef, 0xc0, 0xf3, 0x0f, 0x2a, 0xc6, 0xf3, 0x0f, 0x59, 0xc1, 0xf3, 0x0f, 0x11, 0x00,
    0x0f, 0xbf, 0x72, 0x02, 0x66, 0x0f, 0xef, 0xc0, 0xf3, 0x0f, 0x2a, 0xc6, 0xf3, 0x0f, 0x59, 0xc1,
    0xf3, 0x0f, 0x11, 0x40, 0x04, 0x01, 0xfa, 0x01, 0xc8, 0x83, 0xeb, 0x01, 0x75, 0xcf, 0x5b, 0x5e,
    0x5f, 0x5d, 0xc3, 0x83, 0x7d, 0x24, 0x00, 0x74, 0x41, 0x84, 0xc9, 0x74, 0x3d, 0x8b, 0x4d, 0x18,
    0x85, 0xc9, 0x74, 0xea, 0x8d, 0x0c, 0x1b, 0x01, 0xc9, 0xf3, 0x0f, 0x10, 0x0d, 0x0c, 0x78, 0x03,
    0x00, 0x8b, 0x5d, 0x18, 0x0f, 0xbf, 0x32, 0x66, 0x0f, 0xef, 0xc0, 0xf3, 0x0f, 0x2a, 0xc6, 0xf3,
    0x0f, 0x59, 0xc1, 0xf3, 0x0f, 0x11, 0x40, 0x04, 0xf3, 0x0f, 0x11, 0x00, 0x01, 0xfa, 0x01, 0xc8,
    0x83, 0xeb, 0x01, 0x75, 0xdf, 0x5b, 0x5e, 0x5f, 0x5d, 0xc3, 0x8b, 0x4d, 0x18, 0x85, 0xc9, 0x74,
    0xad, 0x8d, 0x0c, 0x1b, 0x8d, 0x1c, 0x09, 0xf3, 0x0f, 0x10, 0x0d, 0x0c, 0x78, 0x03, 0x00, 0x85,
    0xf6, 0x74, 0x2a, 0x31, 0xc9, 0x83, 0x7d, 0x20, 0x00, 0x74, 0x41, 0x89, 0x7d, 0x0c, 0x0f, 0xbf,
    0x3c, 0x4a, 0x66, 0x0f, 0xef, 0xc0, 0xf3, 0x0f, 0x2a, 0xc7, 0xf3, 0x0f, 0x59, 0xc1, 0xf3, 0x0f,
    0x11, 0x04, 0x88, 0x83, 0xc1, 0x01, 0x39, 0xce, 0x75, 0xe4, 0x8b, 0x7d, 0x0c, 0x8b, 0x4d, 0x24,
    0x85, 0xc9, 0x74, 0x09, 0xf3, 0x0f, 0x10, 0x00, 0xf3, 0x0f, 0x11, 0x40, 0x04, 0x01, 0xfa, 0x01,
    0xd8, 0x83, 0x6d, 0x18, 0x01, 0x75, 0xb8, 0x5b, 0x5e, 0x5f, 0x5d, 0xc3, 0xf3, 0x0f, 0x10, 0x04,
    0x8a, 0xf3, 0x0f, 0x11, 0x04, 0x88, 0x83, 0xc1, 0x01, 0x39, 0xf1, 0x75, 0xef, 0xeb, 0xce,
};
const U32 convertScaleRefs[] = { 0x36, 0x8d, 0xcb }; // movss xmm1, [0x3780c] (0x38000000, 1/32768)

const U8 mixCode[] = { // vnbox_dsound_mix, 0x277d6
    0x55, 0x89, 0xe5, 0x57, 0x56, 0x53, 0x83, 0xec, 0x04, 0x8b, 0x7d, 0x14, 0x8b, 0x55, 0x18, 0x85,
    0xd2, 0x74, 0x4c, 0x8b, 0x45, 0x10, 0x85, 0xc0, 0x74, 0x71, 0x8d, 0x04, 0x3f, 0x01, 0xc0, 0x89,
    0x45, 0xf0, 0x8b, 0x4d, 0x08, 0x01, 0xc1, 0x31, 0xdb, 0x31, 0xf6, 0x85, 0xff, 0x74, 0x21, 0x8d,
    0x04, 0x1b, 0x01, 0xc0, 0x03, 0x45, 0x08, 0x8b, 0x55, 0x18, 0xf3, 0x0f, 0x10, 0x00, 0xf3, 0x0f,
    0x59, 0x02, 0xf3, 0x0f, 0x11, 0x00, 0x83, 0xc0, 0x04, 0x83, 0xc2, 0x04, 0x39, 0xc8, 0x75, 0xea,
    0x83, 0xc6, 0x01, 0x01, 0xfb, 0x8b, 0x45, 0xf0, 0x01, 0xc1, 0x39, 0x75, 0x10, 0x75, 0xcc, 0x0f,
    0xaf, 0x7d, 0x10, 0x89, 0xf9, 0x85, 0xff, 0x74, 0x22, 0x8b, 0x45, 0x0c, 0x8b, 0x55, 0x08, 0x01,
    0xf9, 0x01, 0xc9, 0x01, 0xc1, 0xf3, 0x0f, 0x10, 0x00, 0xf3, 0x0f, 0x58, 0x02, 0xf3, 0x0f, 0x11,
    0x00, 0x83, 0xc0, 0x04, 0x83, 0xc2, 0x04, 0x39, 0xc1, 0x75, 0xea, 0x83, 0xc4, 0x04, 0x5b, 0x5e,
    0x5f, 0x5d, 0xc3,
};

thread_local U32 rngState; // tests run in parallel
U32 rng() { return (rngState = rngState * 1664525 + 1013904223) >> 8; }

void setup() {
    testNewInstruction(0);
    for (int s : { CS, DS, ES, SS }) cpu->seg[s].address = 0; // Wine's code runs flat
    memory->memcpy(CONVERT_FN, convertCode, sizeof(convertCode));
    for (U32 ref : convertScaleRefs) memory->writed(CONVERT_FN + ref, SCALE);
    memory->memcpy(MIX_FN, mixCode, sizeof(mixCode));
    memory->clearPageWriteCounts(CONVERT_FN >> K_PAGE_SHIFT);
    memory->writed(SCALE, 0x38000000);
}

// IN: random 16-bit samples or float bits (both are read as either), the rest floats. Floats include the edges
// (denormals, -0, huge; infinities only in IN, which is copied, so that no inf - inf NaN is mixed).
void fillData(U32 seed) {
    rngState = seed;
    for (U32 a = IN; a < DATA_END; a += 4) {
        U32 r = rng();
        float f = (S16)(rng() & 0xffff) / 32768.0f * (1 + (r & 7));
        U32 bits;
        ::memcpy(&bits, &f, 4);
        switch (r % 16) {
        case 0: bits = (r & 0x100 ? 0x80000000 : 0) | (rng() & 0x7fffff); break; // denormal
        case 1: bits = 0x80000000; break; // -0
        case 2: bits = 0x7f61b1e6; break; // 3e38
        case 3: if (a < OUT) bits = r & 0x100 ? 0x7f800000 : 0xff800000; break;
        case 4: case 5: case 6: if (a < OUT) bits = (rng() & 0xffff) | (rng() << 16); break; // two 16-bit samples
        }
        memory->writed(a, bits);
    }
    memory->writew(IN, 0x8000); // -32768
    memory->writew(IN + 2, 0x7fff);
}

void fillVols(const std::vector<U32>& amp) { // as DSOUND_MixerVol computes them
    for (size_t i = 0; i < amp.size(); i++) {
        float v = amp[i] / ((float)0xFFFF);
        U32 bits;
        ::memcpy(&bits, &v, 4);
        memory->writed(VOLS + (U32)i * 4, bits);
    }
}

std::vector<U8> snapshot() {
    std::vector<U8> m(DATA_END - HEAP);
    memory->memcpy(m.data(), HEAP, (U32)m.size());
    return m;
}

void writeArgs(const std::vector<U32>& args) {
    ESP = STACK;
    memory->writed(STACK, RET);
    for (size_t i = 0; i < 8; i++) memory->writed(STACK + 4 + (U32)i * 4, i < args.size() ? args[i] : 0);
}

// The call as guest code: push the arguments, call, add esp.
void runGuest(U32 fn, const std::vector<U32>& args) {
    ESP = STACK;
    for (size_t i = args.size(); i-- > 0;) {
        testPushCode8(0x68); // push imm32
        testPushCode32(args[i]);
    }
    testPushCode8(0xB8); // mov eax, fn
    testPushCode32(fn == DS_CONVERT ? CONVERT_FN : MIX_FN);
    testPushCode8(0xFF); // call eax
    testPushCode8(0xD0);
    testPushCode8(0x83); // add esp, n
    testPushCode8(0xC4);
    testPushCode8((U8)(args.size() * 4));
    cpu->eip.u32 = TEST_CODE_ADDRESS;
    testRunCPU();
    if (ESP != STACK) testFail("guest call did not balance the stack");
}

bool runHost(U32 fn, const std::vector<U32>& args) {
    writeArgs(args);
    cpu->eip.u32 = 0x1000;
    bool ran = hostDsoundRun(cpu, fn);
    if (ran && (ESP != STACK + 4 || cpu->eip.u32 != RET)) testFail("hosted call did not return like `ret`");
    if (!ran && (ESP != STACK || cpu->eip.u32 != 0x1000)) testFail("declined call changed registers");
    return ran;
}

void same(const char* what, U32 fn, const std::vector<U32>& args, const std::vector<U32>& amp = {}) {
    U32 seed = 0x1234 + (U32)args.size() * 77 + args[args.size() > 4 ? 4 : 2];
    setup();
    fillData(seed);
    fillVols(amp);
    runGuest(fn, args);
    std::vector<U8> guest = snapshot();
    setup();
    fillData(seed);
    fillVols(amp);
    if (!runHost(fn, args)) {
        testFail("%s: hosted call declined", what);
        return;
    }
    std::vector<U8> host = snapshot();
    for (size_t i = 0; i < guest.size(); i++) {
        if (guest[i] != host[i]) {
            testFail("%s: byte at 0x%x is %02x, guest code wrote %02x", what, HEAP + (U32)i, host[i], guest[i]);
            return;
        }
    }
}

void declines(const char* what, U32 fn, const std::vector<U32>& args) {
    setup();
    fillData(99);
    std::vector<U8> before = snapshot();
    if (runHost(fn, args)) testFail("%s: hosted, should run the guest function", what);
    if (snapshot() != before) testFail("%s: declined call wrote memory", what);
}

}

void testHostDsoundSameBytes() {
    const U32 s16 = 1, dup = 1;
    // convert (p, istride, out, ochannels, n, channels, s16, dup)
    same("s16 stereo", DS_CONVERT, { IN, 4, OUT, 2, 600, 2, s16, 0 });
    same("s16 mono to stereo", DS_CONVERT, { IN, 2, OUT, 2, 900, 1, s16, dup });
    same("s16 mono", DS_CONVERT, { IN, 2, OUT, 1, 900, 1, s16, 0 });
    same("s16 5.1", DS_CONVERT, { IN, 12, OUT, 6, 300, 6, s16, 0 });
    same("s16 mono into 2 channels, second left alone", DS_CONVERT, { IN, 2, OUT, 2, 500, 1, s16, 0 });
    same("float stereo", DS_CONVERT, { IN, 8, OUT, 2, 600, 2, 0, 0 });
    same("float mono to stereo", DS_CONVERT, { IN, 4, OUT, 2, 900, 1, 0, dup });
    same("s16 stereo, odd start", DS_CONVERT, { IN + 6, 4, OUT + 8, 2, 333, 2, s16, 0 });
    same("no frames", DS_CONVERT, { IN, 4, OUT, 2, 0, 2, s16, 0 });
    // mix (src, dst, frames, channels, vols)
    same("mix stereo", DS_MIX, { SRC, DST, 900, 2, 0 });
    same("mix stereo with volume", DS_MIX, { SRC, DST, 900, 2, VOLS }, { 0x8000, 0x2d6a });
    same("mix 5.1 with volume", DS_MIX, { SRC, DST, 300, 6, VOLS }, { 0xffff, 0x1, 0x0, 0x7fff, 0x4321, 0xabcd });
    same("mix mono, silent", DS_MIX, { SRC, DST, 1000, 1, VOLS }, { 0 });
    same("mix nothing", DS_MIX, { SRC, DST, 0, 2, VOLS }, { 0x8000, 0x8000 });
}

void testHostDsoundDeclines() {
    declines("convert overlapping", DS_CONVERT, { IN, 4, IN + 8, 2, 100, 2, 1, 0 });
    declines("convert dup with 2 channels", DS_CONVERT, { IN, 4, OUT, 2, 100, 2, 1, 1 });
    declines("convert no channels", DS_CONVERT, { IN, 4, OUT, 2, 100, 0, 1, 0 });
    declines("convert out narrower than a frame", DS_CONVERT, { IN, 4, OUT, 1, 100, 2, 1, 0 });
    declines("convert huge", DS_CONVERT, { IN, 4, OUT, 2, 0x10000000, 2, 1, 0 });
    declines("mix src == dst", DS_MIX, { SRC, SRC, 100, 2, 0 });
    declines("mix vols inside src", DS_MIX, { SRC, DST, 100, 2, SRC + 16 });
    declines("mix frames * channels wraps", DS_MIX, { SRC, DST, 0x80000001, 2, 0 });
    setup();
    fillData(98);
    memory->mprotect(testContext().thread, DST, K_PAGE_SIZE, K_PROT_READ);
    std::vector<U8> before = snapshot();
    if (runHost(DS_MIX, { SRC, DST, 100, 2, 0 })) testFail("mix into a read-only page: hosted");
    if (snapshot() != before) testFail("mix into a read-only page wrote memory");
    memory->mprotect(testContext().thread, DST, K_PAGE_SIZE, K_PROT_READ | K_PROT_WRITE);
}

#endif
