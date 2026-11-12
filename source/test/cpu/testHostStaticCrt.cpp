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

// Static MSVC CRT (hoststaticcrt.cpp): a block starting with one of the known CRT bodies gets the native libc head op and
// gives what that function gives; anything else, or a body changed after it was matched, runs as guest code.
#include "testHostStaticCrt.h"
#include "testCPU.h"
#include "../../emulation/softmmu/kmemory_soft.h"

bool hostLibcRun(CPU* cpu, U32 fn);
const U8* hostStaticCrtTestSig(U32 index, U32& len, U32& kind);
bool hostStaticCrtTestMasked(U32 index, U32 offset);
extern bool hostStaticCrtTestOff;

#define cpu (testContext().cpu)
#define memory (testContext().memory)

namespace {

constexpr U32 HEAP = TEST_HEAP_ADDRESS; // 32 RW pages (testCPU.cpp)
constexpr U32 UNMAPPED = HEAP + 32 * K_PAGE_SIZE;
constexpr U32 STACK = HEAP + 0x10000;
constexpr U32 CODE = TEST_CODE_ADDRESS;
constexpr U32 RET = 0x12345678;
enum { SC_MEMMOVE = 1, SC_MEMSET, SC_STRLEN, SC_STRNCMP }; // hoststaticcrt.cpp

void expect(const char* what, U32 got, U32 want) {
    if (got != want) testFail("%s: got 0x%x, want 0x%x", what, got, want);
}

// Fresh CPU and heap, signature `index` at CODE followed by int3s (a declined call would trap there). Returns its kind.
U32 place(U32 index, U32& len) {
    testNewInstruction(0);
    memory->unmap(UNMAPPED, K_PAGE_SIZE);
    for (int s : { CS, DS, ES, SS }) cpu->seg[s].address = 0;
    memory->memset(HEAP, 0, 32 * K_PAGE_SIZE);
    U32 kind;
    const U8* bytes = hostStaticCrtTestSig(index, len, kind);
    for (U32 i = 0; i < len; i++) testPushCode8(bytes[i]);
    for (U32 i = 0; i < 16; i++) testPushCode8(0xCC);
    cpu->eip.u32 = CODE;
    return kind;
}

// The head op's id when a fresh decode at CODE hooks it, else 0.
U32 hooked(bool flush = true) {
    cpu->eip.u32 = CODE;
    if (flush) memory->removeCode(testContext().thread, CODE, 1, false);
    DecodedOp* op = cpu->getNextOp();
    return op->inst == Custom1 ? op->imm : 0;
}

// The call fn(a0, a1, a2) arriving at CODE, run by the head's hostLibcRun; checks a done call returns like `ret`.
U32 call(U32 fn, U32 a0, U32 a1, U32 a2, bool expectNative, const char* what) {
    ESP = STACK;
    memory->writed(STACK, RET);
    memory->writed(STACK + 4, a0);
    memory->writed(STACK + 8, a1);
    memory->writed(STACK + 12, a2);
    EAX = 0xDEADBEEF;
    ECX = 0x1111;
    EDX = 0x2222;
    cpu->eip.u32 = CODE;
    bool ran = hostLibcRun(cpu, fn);
    if (ran != expectNative) testFail("%s: native %d, expected %d", what, ran, expectNative);
    if (ran && (ESP != STACK + 4 || cpu->eip.u32 != RET)) testFail("%s did not return like `ret`", what);
    if (!ran && (ESP != STACK || cpu->eip.u32 != CODE || EAX != 0xDEADBEEF)) testFail("%s changed registers before falling back", what);
    return EAX;
}

}

// Catches: a known body not hooked, or hooked as the wrong function; memmove's overlap in either direction, zero length,
// and dst == src (MSVC's code still reads and writes both ranges, so an unmapped one must go to the guest); memset's
// byte value and return; strlen over a page boundary; strncmp's unsigned byte difference and n == 0.
void testHostStaticCrtCalls() {
    for (U32 index = 0;; index++) {
        U32 len;
        U32 kind;
        if (!hostStaticCrtTestSig(index, len, kind)) break;
        place(index, len);
        U32 fn = hooked();
        if (!fn) testFail("signature %u: a matching body is not hooked", index);
        U8 ref[64], got[64];
        for (int i = 0; i < 64; i++) ref[i] = (U8)(i + 1);
        U32 src = HEAP + K_PAGE_SIZE - 20; // crosses into the next page
        switch (kind) {
        case SC_MEMMOVE:
            memory->memcpy(src, ref, 64);
            expect("memmove dst > src result", call(fn, src + 7, src, 50, true, "memmove dst > src"), src + 7);
            ::memmove(ref + 7, ref, 50);
            memory->memcpy(got, src, 64);
            if (memcmp(got, ref, 64)) testFail("signature %u: overlap with dst > src differs from memmove", index);
            for (int i = 0; i < 64; i++) ref[i] = (U8)(i + 1);
            memory->memcpy(src, ref, 64);
            call(fn, src, src + 5, 40, true, "memmove dst < src");
            ::memmove(ref, ref + 5, 40);
            memory->memcpy(got, src, 64);
            if (memcmp(got, ref, 64)) testFail("signature %u: overlap with dst < src differs from memmove", index);
            expect("memmove zero length", call(fn, UNMAPPED, UNMAPPED + 8, 0, true, "memmove n=0"), UNMAPPED);
            call(fn, UNMAPPED, UNMAPPED, 8, false, "memmove dst == src unmapped");
            expect("memmove dst == src", call(fn, src, src, 8, true, "memmove dst == src"), src);
            break;
        case SC_MEMSET:
            expect("memset result", call(fn, src, 0x1AB, 6, true, "memset"), src);
            for (U32 i = 0; i < 6; i++) expect("memset byte (low 8 bits of c)", memory->readb(src + i), 0xAB);
            expect("memset outside", memory->readb(src + 6), 0);
            expect("memset zero length", call(fn, UNMAPPED, 1, 0, true, "memset n=0"), UNMAPPED);
            call(fn, UNMAPPED - 4, 1, 8, false, "memset into unmapped");
            break;
        case SC_STRLEN:
            memory->strcpy(HEAP + K_PAGE_SIZE - 5, "hello, world");
            expect("strlen across pages", call(fn, HEAP + K_PAGE_SIZE - 5, 0, 0, true, "strlen"), 12);
            expect("strlen empty", call(fn, HEAP + 0x800, 0, 0, true, "strlen empty"), 0);
            break;
        case SC_STRNCMP:
            memory->strcpy(HEAP, "abc\x80");
            memory->strcpy(HEAP + 0x100, "abc\x01");
            expect("strncmp high byte", call(fn, HEAP, HEAP + 0x100, 4, true, "strncmp"), 0x7F);
            expect("strncmp within n", call(fn, HEAP, HEAP + 0x100, 3, true, "strncmp within n"), 0);
            expect("strncmp prefix", call(fn, HEAP + 0x200, HEAP, 9, true, "strncmp prefix"), (U32)-'a');
            expect("strncmp n=0", call(fn, UNMAPPED, UNMAPPED, 0, true, "strncmp n=0"), 0);
            break;
        default:
            testFail("signature %u: unknown kind %u", index, kind);
        }
    }
}

// Catches: a match that ignores bytes it should check (one changed unmasked byte must stop the hook, at the start, in the
// middle and at the last byte), masked bytes being checked (a relocated address differs in every exe), and a match that
// stays after the guest writes the code: a write past the head's first instruction does not invalidate the decoded
// block, so the call itself must see it, and a write to the first bytes invalidates the block so the decode must see it.
void testHostStaticCrtMatch() {
    for (U32 index = 0;; index++) {
        U32 len;
        U32 kind;
        if (!hostStaticCrtTestSig(index, len, kind)) break;
        const U8* bytes = hostStaticCrtTestSig(index, len, kind);
        for (U32 at = 1; at < len; at++) {
            place(index, len);
            memory->writeb(CODE + at, bytes[at] ^ 0x5A);
            bool masked = hostStaticCrtTestMasked(index, at);
            if ((hooked() != 0) != masked) testFail("signature %u: byte 0x%x changed, hooked %d, masked %d", index, at, !masked, masked);
        }
        // the code page written after the match
        place(index, len);
        U32 fn = hooked();
        if (!fn) testFail("signature %u: not hooked", index);
        U32 mid = 0x10; // unmasked in every signature, past the head's first instruction
        memory->writeb(CODE + mid, bytes[mid] ^ 0x01);
        if (hooked(false) != fn) testFail("signature %u: a write past the head invalidated its block (test assumption)", index);
        call(fn, HEAP, HEAP + 0x100, 0, false, "after a write into the signature");
        memory->writeb(CODE + mid, bytes[mid]);
        call(fn, HEAP, HEAP + 0x100, 0, true, "after the byte was restored");
        memory->writeb(CODE, 0xC3);
        if (hooked(false)) testFail("signature %u: still hooked after its first byte was overwritten", index);
        memory->writeb(CODE, bytes[0]);
        if (!hooked(false)) testFail("signature %u: not hooked again after the first byte was restored", index);
        hostStaticCrtTestOff = true;
        if (hooked()) testFail("signature %u: hooked while turned off", index);
        hostStaticCrtTestOff = false;
    }
}

// Catches: the head op not doing the call through NormalCPU::getOp (memset VC6's int3 tail would trap) or not returning
// to the caller.
void testHostStaticCrtHead() {
    U32 len;
    U32 index = 0;
    for (U32 kind; hostStaticCrtTestSig(index, len, kind) && kind != SC_MEMSET; index++) {}
    place(index, len);
    U32 end = testContext().codeIp; // testRunCPU's end op goes here
    ESP = STACK;
    memory->writed(STACK, end);
    memory->writed(STACK + 4, HEAP + 0x40);
    memory->writed(STACK + 8, 0x77);
    memory->writed(STACK + 12, 5);
    cpu->eip.u32 = CODE;
    testRunCPU();
    expect("result", EAX, HEAP + 0x40);
    expect("stack", ESP, STACK + 4);
    for (U32 i = 0; i < 5; i++) expect("filled", memory->readb(HEAP + 0x40 + i), 0x77);
    expect("past the fill", memory->readb(HEAP + 0x45), 0);
}

#endif
