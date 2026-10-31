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

// Native libc (hosthooks.cpp): each wrapped function must give what the guest's glibc 2.38 code gives
// (same result, same writes, same endptr) or leave the call to the guest, untouched, wherever the guest
// would fault (unmapped, read-only, or a page its word reads could reach).
#include "testHostLibc.h"
#include "testCPU.h"

U32 hostLibcFnId(const char* name);
bool hostLibcRun(CPU* cpu, U32 fn);

#define cpu (testContext().cpu)
#define memory (testContext().memory)

namespace {

constexpr U32 HEAP = 0xF0000000;            // 32 RW pages (testCPU.cpp)
constexpr U32 LAST_PAGE = HEAP + 31 * K_PAGE_SIZE;
constexpr U32 UNMAPPED = HEAP + 32 * K_PAGE_SIZE;
constexpr U32 STACK = HEAP + 0x10000;
constexpr U32 RET = 0x12345678;

void setup() {
    testNewInstruction(0);
    memory->unmap(UNMAPPED, K_PAGE_SIZE);
    for (int s : { CS, DS, ES, SS }) cpu->seg[s].address = 0; // libc code runs flat
    memory->memset(HEAP, 0, 32 * K_PAGE_SIZE);
}

// Calls fn(a0, a1, a2) the way a guest `call` arrives; checks the registers for a done or a declined call.
U32 call(const char* name, U32 a0, U32 a1, U32 a2, bool expectNative) {
    U32 fn = hostLibcFnId(name);
    if (!fn) testFail("%s is not a wrapped function", name);
    ESP = STACK;
    memory->writed(STACK, RET);
    memory->writed(STACK + 4, a0);
    memory->writed(STACK + 8, a1);
    memory->writed(STACK + 12, a2);
    EAX = 0xDEADBEEF;
    cpu->eip.u32 = 0x1000;
    bool ran = hostLibcRun(cpu, fn);
    if (ran != expectNative) testFail("%s(%x, %x, %x): native %d, expected %d", name, a0, a1, a2, ran, expectNative);
    if (ran && (ESP != STACK + 4 || cpu->eip.u32 != RET)) testFail("%s did not return like `ret`", name);
    if (!ran && (ESP != STACK || cpu->eip.u32 != 0x1000 || EAX != 0xDEADBEEF)) testFail("%s changed registers before falling back", name);
    return EAX;
}

void expect(const char* what, U32 got, U32 want) {
    if (got != want) testFail("%s: got 0x%x, want 0x%x", what, got, want);
}

}

// Catches: memmove copying forward over an overlap (dst > src), memcpy not repeating rep movsb's forward
// byte order on an overlap, a page-crossing copy or fill, and a copy that writes the pages it can before
// falling back on a read-only one (the guest then faults with different memory than before).
void testHostLibcCopy() {
    setup();
    U8 ref[64];
    for (int i = 0; i < 64; i++) ref[i] = (U8)(i + 1);
    U32 src = HEAP + K_PAGE_SIZE - 20; // crosses into the next page
    memory->memcpy(src, ref, 64);
    expect("memmove result", call("memmove", src + 7, src, 50, true), src + 7);
    ::memmove(ref + 7, ref, 50);
    U8 got[64];
    memory->memcpy(got, src, 64);
    if (memcmp(got, ref, 64)) testFail("memmove dst > src overlap differs from memmove");

    memory->memcpy(src, ref, 64);
    call("memcpy", src + 1, src, 40, true);
    memory->memcpy(got, src, 64);
    for (int i = 0; i <= 40; i++) if (got[i] != ref[0]) { testFail("memcpy overlap is not rep movsb's forward byte copy"); break; }

    setup();
    expect("memset result", call("memset", HEAP + K_PAGE_SIZE - 3, 0x1AB, 6, true), HEAP + K_PAGE_SIZE - 3);
    for (U32 i = 0; i < 6; i++) expect("memset byte (low 8 bits of c)", memory->readb(HEAP + K_PAGE_SIZE - 3 + i), 0xAB);
    expect("memset outside", memory->readb(HEAP + K_PAGE_SIZE + 3), 0);
    call("memmove", UNMAPPED, UNMAPPED, 100, true); // dst == src: glibc touches nothing
    call("memcpy", UNMAPPED, UNMAPPED, 0, true);    // zero length: nothing read or written

    memory->memset(LAST_PAGE - 8, 0x11, 8);
    memory->mprotect(testContext().thread, LAST_PAGE, K_PAGE_SIZE, K_PROT_READ);
    call("memcpy", LAST_PAGE - 8, HEAP, 16, false);
    call("memset", LAST_PAGE - 8, 0, 16, false);
    memory->mprotect(testContext().thread, LAST_PAGE, K_PAGE_SIZE, K_PROT_READ | K_PROT_WRITE);
    for (U32 i = 0; i < 8; i++) expect("no partial write before a fallback", memory->readb(LAST_PAGE - 8 + i), 0x11);
    call("memcpy", HEAP, UNMAPPED - 4, 8, false); // source runs into an unmapped page
}

// Catches: signed-char compares (0x80 vs 0x01), memcmp's -1/1 (not a byte difference), strcmp's byte
// difference, strncmp reading past n or anything at n == 0, and a compare running into an unmapped page.
void testHostLibcCompare() {
    setup();
    memory->strcpy(HEAP, "abc\x80");
    memory->strcpy(HEAP + 0x100, "abc\x01");
    expect("memcmp high byte", call("memcmp", HEAP, HEAP + 0x100, 4, true), 1);
    expect("memcmp low byte", call("memcmp", HEAP + 0x100, HEAP, 4, true), (U32)-1);
    expect("memcmp n=0", call("memcmp", UNMAPPED, UNMAPPED, 0, true), 0);
    expect("strcmp high byte", call("strcmp", HEAP, HEAP + 0x100, 0, true), 0x7F);
    expect("strcmp prefix", call("strcmp", HEAP + 0x200, HEAP, 0, true), (U32)-'a'); // "" vs "abc\x80"
    expect("strcmp equal", call("strcmp", HEAP, HEAP, 0, true), 0);
    expect("strncmp within n", call("strncmp", HEAP, HEAP + 0x100, 3, true), 0);
    expect("strncmp n=0", call("strncmp", UNMAPPED, UNMAPPED, 0, true), 0);
    // a string running into the unmapped page: the guest faults there, so the guest must run
    memory->memset(LAST_PAGE + K_PAGE_SIZE - 4, 'x', 4);
    memory->memset(HEAP + 0x300, 'x', 8);
    call("strcmp", LAST_PAGE + K_PAGE_SIZE - 4, HEAP + 0x300, 0, false);
    call("memcmp", LAST_PAGE + K_PAGE_SIZE - 4, HEAP + 0x300, 8, false);
}

// Catches: strlen/strnlen over a page boundary, strnlen stopping at n, memchr at n == 0 touching memory,
// strchr not truncating c to a char or missing the terminator for c == 0, strrchr returning the first hit,
// and a scan that succeeds where glibc's aligned word reads would fault in the next, unmapped page.
void testHostLibcScan() {
    setup();
    U32 s = HEAP + K_PAGE_SIZE - 5;
    memory->strcpy(s, "hello, world");
    expect("strlen across pages", call("strlen", s, 0, 0, true), 12);
    expect("strnlen limit", call("strnlen", s, 4, 0, true), 4);
    expect("strnlen short", call("strnlen", s, 100, 0, true), 12);
    expect("memchr hit", call("memchr", s, 'w', 12, true), s + 7);
    expect("memchr miss", call("memchr", s, 'w', 7, true), 0);
    expect("memchr n=0", call("memchr", UNMAPPED, 0, 0, true), 0);
    expect("strchr char of c", call("strchr", s, 0x100 + 'o', 0, true), s + 4);
    expect("strchr terminator", call("strchr", s, 0, 0, true), s + 12);
    expect("strchr miss", call("strchr", s, 'z', 0, true), 0);
    expect("strrchr last", call("strrchr", s, 'o', 0, true), s + 8);
    expect("strrchr terminator", call("strrchr", s, 0, 0, true), s + 12);
    expect("strrchr miss", call("strrchr", s, 'z', 0, true), 0);
    // terminator in the last 2 bytes of the last mapped page: glibc's next word read may reach the unmapped page
    memory->strcpy(LAST_PAGE + K_PAGE_SIZE - 2, "a");
    call("strlen", LAST_PAGE + K_PAGE_SIZE - 2, 0, 0, false);
    memory->memset(LAST_PAGE + K_PAGE_SIZE - 16, 'a', 16);
    call("strlen", LAST_PAGE + K_PAGE_SIZE - 16, 0, 0, false); // no terminator before the unmapped page
    memory->strcpy(HEAP + 0x400, "copy me");
    expect("strcpy result", call("strcpy", HEAP + 0x500, HEAP + 0x400, 0, true), HEAP + 0x500);
    expect("strcpy copied", call("strcmp", HEAP + 0x500, HEAP + 0x400, 0, true), 0);
    call("strcpy", HEAP + 0x402, HEAP + 0x400, 0, false); // overlapping (undefined in C): the guest's own result
}

// Catches the strtol edge cases glibc defines: "0x" with no digit ends at the x, C23 "0b" only in
// __isoc23_*, strtoul("-1") wrapping, ERANGE and EINVAL left to the guest (errno), whitespace, empty input.
void testHostLibcStrtol() {
    struct Case { const char* fn; const char* s; S32 base; bool native; U32 value; U32 end; };
    static const Case cases[] = {
        {"strtol", "  -42xyz", 10, true, (U32)-42, 5},
        {"strtol", "0x1A", 0, true, 26, 4},
        {"strtoul", "0x", 16, true, 0, 1},
        {"strtoul", " 0xg", 0, true, 0, 2},
        {"strtol", "077", 0, true, 63, 3},
        {"strtol", "0b101", 0, true, 0, 1},
        {"__isoc23_strtol", "0b101", 0, true, 5, 5},
        {"__isoc23_strtoul", "0b", 2, true, 0, 1},
        {"__isoc23_strtoul", "0x10", 0, true, 16, 4},
        {"strtoul", "-1", 10, true, 0xFFFFFFFF, 2},
        {"strtoul", "4294967295", 10, true, 0xFFFFFFFF, 10},
        {"strtoul", "4294967296", 10, false, 0, 0},
        {"strtol", "2147483648", 10, false, 0, 0},
        {"strtol", "-2147483648", 10, true, 0x80000000, 11},
        {"strtol", "zZ", 36, true, 35 * 36 + 35, 2},
        {"strtol", "", 10, true, 0, 0},
        {"strtol", " \t\n", 10, true, 0, 0},
        {"strtol", "-", 10, true, 0, 0},
        {"strtol", "12", 1, false, 0, 0},
        {"strtol", "12", 37, false, 0, 0},
    };
    for (const Case& c : cases) {
        setup();
        U32 s = HEAP + K_PAGE_SIZE - 4; // the digits cross a page boundary
        memory->strcpy(s, c.s);
        U32 endptr = HEAP + 0x800;
        memory->writed(endptr, 0xCCCCCCCC);
        U32 value = call(c.fn, s, endptr, (U32)c.base, c.native);
        if (!c.native) {
            expect(c.s, memory->readd(endptr), 0xCCCCCCCC);
            continue;
        }
        expect(c.s, value, c.value);
        expect(c.s, memory->readd(endptr) - s, c.end);
    }
    setup();
    memory->strcpy(HEAP, "123");
    expect("strtol without endptr", call("strtol", HEAP, 0, 10, true), 123);
    call("strtol", HEAP, UNMAPPED, 10, false); // endptr store would fault
}

#endif
