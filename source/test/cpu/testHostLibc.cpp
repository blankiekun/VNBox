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
#include "ksignal.h"
#include "../../emulation/softmmu/kmemory_soft.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

U32 hostLibcFnId(const char* name);
U32 hostCrtFnId(const char* name);
std::vector<std::pair<U64, U32>> hostCrtParseTest(KFile* file);
bool hostLibcRun(CPU* cpu, U32 fn);
bool hostClockRun(CPU* cpu, U32 fn); // hostclock.cpp, fn: 1 NtQueryPerformanceCounter, 2 NtQuerySystemTime, 3 RtlGetSystemTimePrecise

#define cpu (testContext().cpu)
#define memory (testContext().memory)

extern thread_local U32 hostLibcTestAddress, hostLibcTestFn;
extern "C" U32 bw_hook_add(U32 pid, U32 eip); // hosthooks.cpp
extern "C" void bw_hook_remove(U32 id);

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
// crt: the Wine CRT DLLs' export of that name (hostCrtFnId), not glibc's.
U32 call(const char* name, U32 a0, U32 a1, U32 a2, bool expectNative, U32 eip = 0x1000, bool crt = false) {
    U32 fn = crt ? hostCrtFnId(name) : hostLibcFnId(name);
    if (!fn) testFail("%s is not a wrapped function", name);
    ESP = STACK;
    memory->writed(STACK, RET);
    memory->writed(STACK + 4, a0);
    memory->writed(STACK + 8, a1);
    memory->writed(STACK + 12, a2);
    EAX = 0xDEADBEEF;
    cpu->eip.u32 = eip;
    bool ran = hostLibcRun(cpu, fn);
    if (ran != expectNative) testFail("%s(%x, %x, %x): native %d, expected %d", name, a0, a1, a2, ran, expectNative);
    if (ran && (ESP != STACK + 4 || cpu->eip.u32 != RET)) testFail("%s did not return like `ret`", name);
    if (!ran && (ESP != STACK || cpu->eip.u32 != eip || EAX != 0xDEADBEEF)) testFail("%s changed registers before falling back", name);
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
    expect("memmove dst == src result", call("memmove", UNMAPPED, UNMAPPED, 100, true), UNMAPPED); // glibc touches nothing and returns dst
    call("memcpy", UNMAPPED, UNMAPPED, 0, true);    // zero length: nothing read or written

    memory->memset(LAST_PAGE - 8, 0x11, 8);
    memory->mprotect(testContext().thread, LAST_PAGE, K_PAGE_SIZE, K_PROT_READ);
    call("memcpy", LAST_PAGE - 8, HEAP, 16, false);
    call("memset", LAST_PAGE - 8, 0, 16, false);
    memory->mprotect(testContext().thread, LAST_PAGE, K_PAGE_SIZE, K_PROT_READ | K_PROT_WRITE);
    for (U32 i = 0; i < 8; i++) expect("no partial write before a fallback", memory->readb(LAST_PAGE - 8 + i), 0x11);
    call("memcpy", HEAP, UNMAPPED - 4, 8, false); // source runs into an unmapped page
}

// Catches: a hooked export reached while a debug trap is active (hardware breakpoint set, or TF) raising SIGILL instead
// of running the guest function, the fallback running on past the single step, and TF's trap landing anywhere but
// after that one op. The head op is the real one (NormalCPU::getOp), at an address hostLibcAt is told about.
void testHostLibcDebugTrap() {
    constexpr U32 CODE = TEST_CODE_ADDRESS, OP2 = CODE + 5, HANDLER = CODE + 0x40;
    for (bool useTf : { false, true }) {
        setup();
        KSigAction& ill = testContext().process->sigActions[K_SIGILL];
        KSigAction& trap = testContext().process->sigActions[K_SIGTRAP];
        ill.reset();
        trap.reset();
        ill.handlerAndSigAction = trap.handlerAndSigAction = HANDLER;
        testPushCode8(0xB8); // mov eax, 0x11223344: the guest function's first op
        for (U8 b : { 0x44, 0x33, 0x22, 0x11 }) testPushCode8(b);
        testPushCode8(0x43); // inc ebx: must wait for the next step
        hostLibcTestAddress = CODE;
        hostLibcTestFn = hostLibcFnId("strlen");
        ESP = STACK;
        cpu->eip.u32 = CODE;
        if (useTf) {
            cpu->fillFlags();
            cpu->setFlags(TF, TF);
        } else {
            testContext().thread->debugRegs[0] = HEAP + 0x800; // a write watchpoint nothing here touches
            testContext().thread->debugRegs[7] = 3 | (1u << 16) | (3u << 18);
            testContext().thread->updateDebugTrapActive();
        }
        cpu->nextOp = cpu->getNextOp();
        if (cpu->nextOp->inst != Custom1) testFail("no head op at the hooked address");
        cpu->run();
        hostLibcTestFn = 0;
        if (ill.sigInfo[0] == K_SIGILL) testFail("hooked export under a debug trap raised SIGILL (tf %d)", useTf);
        if (useTf) { // the handler was entered: what the op left is in the signal's saved context
            if (trap.sigInfo[0] != K_SIGTRAP) testFail("TF over a hooked export: no SIGTRAP");
            U32 context = memory->readd(ESP + 12);
            expect("first guest op ran", memory->readd(context + 0x40), 0x11223344);
            expect("second guest op waits", memory->readd(context + 0x34), 0);
            expect("TF trap eip", memory->readd(context + 0x4c), OP2);
            expect("handler entered", cpu->eip.u32, HANDLER);
        } else {
            expect("first guest op ran", EAX, 0x11223344);
            expect("second guest op waits", EBX, 0);
            expect("eip after one op", cpu->eip.u32, OP2);
        }
        ill.reset();
        trap.reset();
        cpu->debugTrapOnNextInstruction = false;
        cpu->setFlags(0, TF);
        testContext().thread->debugRegs[7] = 0;
        testContext().thread->updateDebugTrapActive();
    }
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


// Native clock (hostclock.cpp) must give the values Wine's unix side computed from the clock_gettime64 syscall
// (ticks = 100 ns; QPC from CLOCK_BOOTTIME, system time from CLOCK_REALTIME since 1601), return like `ret 8` /
// `ret 4` / `ret`, and leave an unwritable pointer to the guest. Catches a wrong unit (us vs 100 ns), a missing
// 1601 epoch, a stdcall return that doesn't pop the arguments, and a write before the fallback.
void testHostClock() {
    auto clockTicks = [](U32 clockId) { // what Wine computes from the syscall: tv_sec * 10^7 + tv_nsec / 100
        U32 ts = HEAP + 0x900;
        KSystem::clock_gettime64(testContext().thread, clockId, ts);
        return memory->readq(ts) * 10000000 + memory->readd(ts + 8) / 100;
    };
    auto run = [](U32 fn, U32 a0, U32 a1, bool expectNative, U32 pop) {
        ESP = STACK;
        memory->writed(STACK, RET);
        memory->writed(STACK + 4, a0);
        memory->writed(STACK + 8, a1);
        EAX = 0xDEADBEEF;
        cpu->eip.u32 = 0x1000;
        bool ran = hostClockRun(cpu, fn);
        if (ran != expectNative) testFail("clock %d: native %d, expected %d", fn, ran, expectNative);
        if (ran && (ESP != STACK + 4 + pop || cpu->eip.u32 != RET)) testFail("clock %d did not return like `ret %d`", fn, pop);
        if (!ran && (ESP != STACK || cpu->eip.u32 != 0x1000 || EAX != 0xDEADBEEF)) testFail("clock %d changed registers before falling back", fn);
    };
    setup();
    U32 counter = HEAP + 0x100, freq = HEAP + 0x108;
    U64 before = clockTicks(7); // CLOCK_BOOTTIME
    run(1, counter, freq, true, 8);
    U64 after = clockTicks(7);
    U64 qpc = memory->readq(counter);
    if (qpc < before || qpc > after) testFail("QPC %llu not within the syscall's %llu..%llu", qpc, before, after);
    expect("QPC status", EAX, 0);
    if (memory->readq(freq) != 10000000) testFail("QPC frequency %llu", memory->readq(freq));
    memory->writeq(freq, 0);
    run(1, counter, 0, true, 8); // no frequency asked
    if (memory->readq(freq) != 0) testFail("QPC wrote a null frequency pointer");
    run(1, UNMAPPED, freq, false, 0);
    run(1, counter, UNMAPPED, false, 0);
    if (memory->readq(freq) != 0) testFail("QPC wrote before falling back");

    U32 time = HEAP + 0x110;
    before = clockTicks(5) + 116444736000000000ull; // CLOCK_REALTIME_COARSE, the clock NtQuerySystemTime picks
    run(2, time, 0, true, 4);
    after = clockTicks(5) + 116444736000000000ull;
    U64 t = memory->readq(time);
    if (t < before || t > after) testFail("system time %llu not within the syscall's %llu..%llu", t, before, after);
    run(2, UNMAPPED, 0, false, 0);

    before = clockTicks(0) + 116444736000000000ull; // CLOCK_REALTIME
    run(3, 0, 0, true, 0);
    t = ((U64)EDX << 32) | EAX;
    after = clockTicks(0) + 116444736000000000ull;
    if (t < before || t > after) testFail("precise system time %llu not within the syscall's %llu..%llu", t, before, after);
}

// Native mutexes (hostmutex.cpp). The functions find libc's data from their own address: these eips put
// __pthread_force_elision of that libc build into the test heap.
namespace {
constexpr U32 SYM_LOCK = 0x721f5, SYM_UNLOCK = 0x737b3, FORCE_ELISION = 0x19391c;
constexpr U32 ELISION_AT = HEAP + 0x8000, TCB = HEAP + 0xA000, MTX = HEAP + 0xC000;
constexpr U32 TID = 4242;
constexpr U32 LOCK_EIP = ELISION_AT - FORCE_ELISION + SYM_LOCK, UNLOCK_EIP = ELISION_AT - FORCE_ELISION + SYM_UNLOCK;

void mutexSetup(U32 kind, U32 lock, U32 count, U32 owner) {
    setup();
    cpu->seg[GS].address = TCB;
    memory->writed(TCB + 0x68, TID); // tid
    memory->writed(MTX, lock);
    memory->writed(MTX + 4, count);
    memory->writed(MTX + 8, owner);
    memory->writed(MTX + 12, kind);
    memory->writed(MTX + 16, 5); // __nusers
}

void expectMutex(const char* what, U32 lock, U32 count, U32 owner, U32 nusers) {
    expect(what, memory->readd(MTX), lock);
    expect(what, memory->readd(MTX + 4), count);
    expect(what, memory->readd(MTX + 8), owner);
    expect(what, memory->readd(MTX + 16), nusers);
}

U32 lock(bool native) { return call("pthread_mutex_lock", MTX, 0, 0, native, LOCK_EIP); }
U32 unlock(bool native) { return call("pthread_mutex_unlock", MTX, 0, 0, native, UNLOCK_EIP); }
}

// Catches: a lock taken when the word isn't free (the guest has to futex-wait), an unlock that zeroes a word
// with waiters (their wake-up is lost), a recursive count or owner/nusers bookkeeping that differs from glibc,
// errorcheck/recursive error codes, and a fast path taken for a kind glibc handles differently (pshared,
// robust, PI, elision forced) or past glibc's owner assertion. A declined call must write nothing.
void testHostLibcMutex() {
    for (U32 kind : { 0u, 3u }) { // normal, adaptive
        mutexSetup(kind, 0, 0, 0);
        expect("lock free", lock(true), 0);
        expectMutex("locked", 1, 0, TID, 6);
        lock(false); // held (by us: a normal mutex deadlocks in glibc too)
        expectMutex("relock declined", 1, 0, TID, 6);
        expect("unlock", unlock(true), 0);
        expectMutex("unlocked", 0, 0, 0, 5);
        mutexSetup(kind, 2, 0, TID);
        unlock(false);
        expectMutex("unlock with waiters declined", 2, 0, TID, 5);
        mutexSetup(kind, 0, 0, 77);
        lock(false);
        expectMutex("owner set on a free word declined", 0, 0, 77, 5);
    }
    mutexSetup(0, 0, 0, 0);
    memory->writed(ELISION_AT, 1);
    lock(false);
    expectMutex("forced elision declined", 0, 0, 0, 5);

    mutexSetup(1, 0, 0, 0); // recursive
    expect("rlock", lock(true), 0);
    expectMutex("rlocked", 1, 1, TID, 6);
    expect("rlock again", lock(true), 0);
    expectMutex("rlocked twice", 1, 2, TID, 6);
    expect("runlock", unlock(true), 0);
    expectMutex("still held", 1, 1, TID, 6);
    expect("runlock last", unlock(true), 0);
    expectMutex("runlocked", 0, 0, 0, 5);
    mutexSetup(1, 1, 1, 99);
    expect("runlock not owner", unlock(true), 1);
    lock(false); // held by another thread
    expectMutex("other owner untouched", 1, 1, 99, 5);
    mutexSetup(1, 1, 0xFFFFFFFF, TID);
    expect("rlock count overflow", lock(true), 11);
    mutexSetup(1, 2, 1, TID);
    unlock(false);
    expectMutex("runlock with waiters declined", 2, 1, TID, 5);

    mutexSetup(2, 1, 0, TID); // errorcheck
    expect("relock errorcheck", lock(true), 35);
    expect("eunlock", unlock(true), 0);
    expectMutex("eunlocked", 0, 0, 0, 4); // set up locked with __nusers 5
    expect("eunlock unlocked", unlock(true), 1);
    mutexSetup(2, 1, 0, 99);
    expect("eunlock not owner", unlock(true), 1);
    expectMutex("errorcheck untouched", 1, 0, 99, 5);

    for (U32 kind : { 0x80u, 0x81u, 0x10u, 0x20u, 0x40u, 0x100u }) { // pshared, robust, PI, PP, elision
        mutexSetup(kind, 0, 0, 0);
        lock(false);
        mutexSetup(kind, 1, 0, TID);
        unlock(false);
        expectMutex("special kind untouched", 1, 0, TID, 5);
    }
}

// A private file mapping of two pages holding `code` from offset 0: hostHookPatched compares against this file, and its
// pages start out File (not loaded), then CopyOnWrite once read.
static void mapFile(U32 address, const U8* code, U32 len) {
    KProcessPtr process = testContext().process;
    U32 fd = process->memfd_create(B("hostlibc-test"), 0);
    if ((S32)fd < 0) testFail("memfd_create failed");
    std::shared_ptr<KFile> file = std::dynamic_pointer_cast<KFile>(process->getFileDescriptor(fd)->kobject);
    if (process->ftruncate64(fd, 2 * K_PAGE_SIZE)) testFail("ftruncate failed");
    if (file->pwriteNative(const_cast<U8*>(code), 0, len) != len) testFail("file write failed");
    if (memory->mmap(testContext().thread, address, 2 * K_PAGE_SIZE, K_PROT_READ | K_PROT_WRITE | K_PROT_EXEC, K_MAP_FIXED | K_MAP_PRIVATE, fd, 0) != address) testFail("mmap failed");
    process->close(fd);
}

// Catches: a game's jmp written over a hooked export being ignored (the hook answers, its code never runs), a restored
// export staying off, and the compare being too wide or too eager and turning hooks off: on bytes past the fifth (Wine's
// syscall thunk has a relocated address there) or on a page not loaded yet. Uses the real head op from NormalCPU::getOp.
void testHostHookPatchedExport() {
    constexpr U32 MAPPED = 0x20000000, CROSS = MAPPED + K_PAGE_SIZE - 2;
    setup();
    U8 code[K_PAGE_SIZE];
    ::memset(code, 0x90, sizeof(code));
    const U8 mov[] = { 0xB8, 0x44, 0x33, 0x22, 0x11 }; // mov eax, 0x11223344
    ::memcpy(code, mov, sizeof(mov));
    mapFile(MAPPED, code, sizeof(code));
    hostLibcTestFn = hostLibcFnId("strlen");
    hostLibcTestAddress = MAPPED;
    cpu->seg[CS].address = 0;
    cpu->eip.u32 = MAPPED;
    auto hooked = [](const char* what, bool want, bool flush = true) {
        if (flush) memory->removeCode(testContext().thread, cpu->eip.u32, 1, false);
        DecodedOp* op = cpu->getNextOp();
        if ((op->inst == Custom1) != want) testFail("%s: hooked %d, expected %d", what, op->inst == Custom1, want);
    };
    hooked("page not loaded yet", true);
    hooked("same bytes in RAM", true);
    memory->writeb(MAPPED + 5, 0xCC);
    hooked("sixth byte differs", true);
    memory->writeb(MAPPED, 0xE9);
    hooked("first byte overwritten, the write itself invalidates", false, false);
    memory->writeb(MAPPED, 0xB8);
    hooked("first byte restored, the write itself invalidates", true, false);
    memory->writeb(MAPPED + 4, 0x12);
    hooked("fifth byte overwritten", false);
    memory->writeb(MAPPED + 4, 0x11);

    hostLibcTestAddress = cpu->eip.u32 = CROSS;
    hooked("five bytes over a page not loaded yet", true);
    memory->writeb(MAPPED + K_PAGE_SIZE, 0xCC);
    hooked("overwritten on the next page", false);
    hostLibcTestFn = 0;
    memory->unmap(MAPPED, 2 * K_PAGE_SIZE);
}

// Catches: a change in getOp or the head's fields that breaks hooks while the direct hostLibcRun tests still pass: the head
// not doing the call and returning (the guest function would run), or a decline not going on with the guest function
// after its first op (the real op's next, which the head op holds on to). The trap case is testHostLibcDebugTrap.
void testHostLibcHead() {
    for (bool decline : { false, true }) {
        setup();
        testPushCode8(0xB8); // mov eax, 0x11223344: the guest function's first op
        for (U8 b : { 0x44, 0x33, 0x22, 0x11 }) testPushCode8(b);
        testPushCode8(0x43); // inc ebx: its second, then testRunCPU's end op, where the return address points
        memory->writeb(HEAP + 0x100, 'a');
        memory->writeb(HEAP + 0x101, 'b');
        hostLibcTestAddress = TEST_CODE_ADDRESS;
        hostLibcTestFn = hostLibcFnId("strlen");
        ESP = STACK;
        memory->writed(STACK, TEST_CODE_ADDRESS + 6);
        memory->writed(STACK + 4, decline ? UNMAPPED : HEAP + 0x100);
        cpu->eip.u32 = TEST_CODE_ADDRESS;
        testRunCPU();
        hostLibcTestFn = 0;
        expect("result", EAX, decline ? 0x11223344 : 2);
        expect("the guest function's second op", EBX, decline ? 1 : 0);
        expect("stack", ESP, decline ? STACK : STACK + 4);
    }
}

// Catches: removing any test from hostLibcRun's gate or libcRam: a rep movsb with DF set copying forward in the host and
// backward in the guest, a segment base ignored, a write that skips copy-on-write or a decoded code page's
// invalidation, a page read before it is loaded. Each must leave the call to the guest with nothing changed.
void testHostLibcDeclines() {
    constexpr U32 DST = HEAP + 0x100, SRC = HEAP + 0x200, MAPPED = 0x20000000;
    setup();
    memory->memset(SRC, 0x5A, 16);
    auto bothDecline = [](U32 dst, U32 src) {
        U8 before[16], after[16];
        memory->memcpy(before, dst, 16);
        call("memcpy", dst, src, 16, false);
        call("memset", dst, 0x77, 16, false);
        memory->memcpy(after, dst, 16);
        if (::memcmp(before, after, 16)) testFail("a declined call changed memory at %x", dst);
    };

    cpu->flags |= DF;
    bothDecline(DST, SRC);
    cpu->flags &= ~DF;
    for (int s : { CS, DS, ES, SS }) {
        cpu->seg[s].address = 0x1000;
        bothDecline(DST, SRC);
        cpu->seg[s].address = 0;
    }
    cpu->stackMask = 0xFFFF;
    bothDecline(DST, SRC);
    cpu->stackMask = 0xFFFFFFFF;

    testPushCode8(0x90); // a decoded page: a host write would miss the invalidation
    cpu->eip.u32 = TEST_CODE_ADDRESS;
    cpu->getNextOp();
    if (getMemData(memory)->mmu[TEST_CODE_ADDRESS >> K_PAGE_SHIFT].getPageType() != PageType::Code) testFail("the code page isn't PageType::Code");
    bothDecline(TEST_CODE_ADDRESS + 0x100, SRC);

    U8 data[16];
    ::memset(data, 0x33, sizeof(data));
    mapFile(MAPPED, data, sizeof(data));
    call("memcpy", DST, MAPPED, 16, false); // not loaded: the guest's read loads it
    call("memset", MAPPED, 0x77, 16, false);
    expect("page not loaded", (U32)getMemData(memory)->mmu[MAPPED >> K_PAGE_SHIFT].getPageType(), (U32)PageType::File);
    memory->readb(MAPPED);
    expect("page loaded", (U32)getMemData(memory)->mmu[MAPPED >> K_PAGE_SHIFT].getPageType(), (U32)PageType::CopyOnWrite);
    call("memset", MAPPED, 0x77, 16, false); // a host write would land in the file cache's page
    call("memcpy", MAPPED, SRC, 16, false);
    expect("copy-on-write page untouched", memory->readb(MAPPED + 8), 0x33);
    call("memcpy", DST, MAPPED, 16, true); // reading it is fine
    expect("read from a copy-on-write page", memory->readb(DST + 8), 0x33);
    memory->unmap(MAPPED, 2 * K_PAGE_SIZE);
}

// Wine's CRT DLLs (dlls/msvcrt/string.c) must give what that code gives. Catches: memcpy copying forward over an overlap (msvcrt's
// memcpy is memmove, glibc's here is rep movsb), strstr missing a match a naive restart skips, not taking the first match, reading
// past a page or the haystack for an empty needle, _strnicmp folding to upper case ('[' sorts after 'A' that way) or comparing
// past n or the terminator, and every case its locale branch or invalid parameter handler decides left to the guest.
void testHostCrt() {
    setup();
    auto crt = [](const char* name, U32 a0, U32 a1, U32 a2, bool expectNative) { return call(name, a0, a1, a2, expectNative, 0x1000, true); };
    U8 ref[64], got[64];
    for (int i = 0; i < 64; i++) ref[i] = (U8)(i + 1);
    U32 src = HEAP + K_PAGE_SIZE - 20; // crosses into the next page
    for (int shift : { 7, -7 }) {
        memory->memcpy(src, ref, 64);
        U32 dst = src + 8 + shift;
        expect("memcpy result", crt("memcpy", dst, src + 8, 40, true), dst);
        U8 want[64];
        ::memcpy(want, ref, 64);
        ::memmove(want + 8 + shift, want + 8, 40);
        memory->memcpy(got, src, 64);
        if (memcmp(got, want, 64)) testFail("CRT memcpy over an overlap (shift %d) is not memmove", shift);
    }
    expect("memset", crt("memset", HEAP + 0x40, 0x1CC, 3, true), HEAP + 0x40);
    expect("memset byte", memory->readb(HEAP + 0x42), 0xCC);

    U32 s = HEAP + K_PAGE_SIZE - 5;
    memory->strcpy(s, "hello, world");
    expect("strlen", crt("strlen", s, 0, 0, true), 12);
    memory->strcpy(HEAP + 0x100, "o, w");
    memory->strcpy(HEAP + 0x110, "world");
    memory->strcpy(HEAP + 0x120, "worlds");
    memory->strcpy(HEAP + 0x130, "o");
    expect("strstr across pages", crt("strstr", s, HEAP + 0x100, 0, true), s + 4);
    expect("strstr at the end", crt("strstr", s, HEAP + 0x110, 0, true), s + 7);
    expect("strstr first match", crt("strstr", s, HEAP + 0x130, 0, true), s + 4);
    expect("strstr past the terminator", crt("strstr", s, HEAP + 0x120, 0, true), 0);
    expect("strstr empty needle", crt("strstr", UNMAPPED, HEAP + 0x200, 0, true), UNMAPPED); // the haystack isn't read
    memory->strcpy(HEAP + 0x300, "abcabcabd");
    memory->strcpy(HEAP + 0x320, "abcabd");
    expect("strstr partial match restart", crt("strstr", HEAP + 0x300, HEAP + 0x320, 0, true), HEAP + 0x303);
    memory->strcpy(HEAP + 0x340, "aaaaab");
    memory->strcpy(HEAP + 0x360, "aaab");
    expect("strstr repeated prefix", crt("strstr", HEAP + 0x340, HEAP + 0x360, 0, true), HEAP + 0x342);
    expect("strstr empty haystack", crt("strstr", HEAP + 0x200, HEAP + 0x360, 0, true), 0);
    memory->memset(LAST_PAGE + K_PAGE_SIZE - 16, 'a', 16);
    crt("strstr", LAST_PAGE + K_PAGE_SIZE - 16, HEAP + 0x110, 0, false); // the haystack runs into the unmapped page
    crt("strstr", HEAP + 0x300, LAST_PAGE + K_PAGE_SIZE - 16, 0, false); // so does the needle

    memory->strcpy(HEAP + 0x400, "Hello[");
    memory->strcpy(HEAP + 0x410, "hELLOa");
    memory->strcpy(HEAP + 0x420, "hELLO");
    memory->strcpy(HEAP + 0x430, "ab\x82");
    memory->strcpy(HEAP + 0x440, "ax\x82");
    memory->strcpy(HEAP + 0x450, "ay\x82");
    expect("_strnicmp equal", crt("_strnicmp", HEAP + 0x400, HEAP + 0x410, 5, true), 0);
    expect("_strnicmp lowers", crt("_strnicmp", HEAP + 0x400, HEAP + 0x410, 6, true), (U32)('[' - 'a'));
    expect("_strnicmp terminator", crt("_strnicmp", HEAP + 0x420, HEAP + 0x400, 100, true), (U32)-'[');
    expect("_strnicmp n=0", crt("_strnicmp", 0, 0, 0, true), 0);
    expect("_strnicmp stops at n", crt("_strnicmp", HEAP + 0x430, HEAP + 0x430, 2, true), 0);
    expect("_strnicmp stops at a difference", crt("_strnicmp", HEAP + 0x440, HEAP + 0x450, 3, true), (U32)-1);
    crt("_strnicmp", HEAP + 0x430, HEAP + 0x430, 3, false); // a byte from 0x80: the locale's tables decide
    crt("_strnicmp", 0, HEAP + 0x400, 3, false);          // NULL: the invalid parameter handler
    crt("_strnicmp", HEAP + 0x400, 0, 3, false);
    crt("_strnicmp", HEAP + 0x400, HEAP + 0x410, 0x80000000, false); // count > INT_MAX: the same, from msvcr80 on
    crt("_strnicmp", LAST_PAGE + K_PAGE_SIZE - 16, LAST_PAGE + K_PAGE_SIZE - 16, 100, false); // runs into the unmapped page

    cpu->flags |= DF; // the shared gate (testHostLibcDeclines) holds for these too
    memory->memcpy(src, ref, 64);
    crt("memcpy", src + 1, src, 16, false);
    crt("strstr", s, HEAP + 0x100, 0, false);
    cpu->flags &= ~DF;
    memory->memcpy(got, src, 64);
    if (memcmp(got, ref, 64)) testFail("a declined CRT memcpy changed memory");
}

// Catches: the CRT exports looked for in .dynsym (Wine's builtins have none there), a function address read as an RVA from the
// NT header (it is the ELF address) or mapped to a file offset without its section's address-offset gap, a forwarder or a
// function that isn't wrapped taken, and an ordinal past the function table read. A made-up builtin, laid out like winebuild's.
void testHostCrtExports() {
    constexpr U32 TEXT = 0x1000, DATA_OFFSET = 0x2000, DATA = 0x3000, PE_NT = DATA, EXPORTS = 0x100, SH = 0x2a00;
    std::vector<U8> f(SH + 5 * 40);
    auto put32 = [&](U32 at, U32 v) { for (int i = 0; i < 4; i++) f[at + i] = (U8)(v >> (i * 8)); };
    auto put16 = [&](U32 at, U32 v) { f[at] = (U8)v; f[at + 1] = (U8)(v >> 8); };
    auto data = [&](U32 rva) { return DATA_OFFSET + rva; }; // file offset of an RVA from the NT header
    ::memcpy(&f[0], "\x7f" "ELF\x01\x01", 6);
    put16(18, 3); // EM_386
    put32(32, SH);
    put16(46, 40);
    put16(48, 5);
    auto section = [&](U32 i, U32 type, U32 flags, U32 addr, U32 offset, U32 size, U32 link) {
        U32 s = SH + i * 40;
        put32(s + 4, type); put32(s + 8, flags); put32(s + 12, addr); put32(s + 16, offset); put32(s + 20, size); put32(s + 24, link);
    };
    section(1, 1, 6, TEXT, TEXT, 0x100, 0);               // .text: SHF_ALLOC | SHF_EXECINSTR
    section(2, 1, 3, DATA, DATA_OFFSET, 0x800, 0);        // .data, 0x1000 above its file offset like the real ones
    section(3, 11, 2, 0x200, 0x2800, 32, 4);              // .dynsym: the null symbol, __wine_spec_nt_header
    section(4, 3, 2, 0x300, 0x2900, 23, 0);               // .dynstr
    ::memcpy(&f[0x2901], "__wine_spec_nt_header", 21);
    put32(0x2810, 1);
    put32(0x2814, PE_NT);
    put16(0x281e, 2);
    put32(data(0), 0x4550);
    put16(data(24), 0x10b);
    put32(data(24 + 96), EXPORTS);
    put32(data(24 + 100), 0x200);
    const char* names[] = { "memcpy", "_strnicmp", "strstr", "strcmp", "memset" };
    const U32 functions[] = { TEXT + 0x10, TEXT + 0x20, DATA + 0x2c0, TEXT + 0x30 }; // strstr: a forwarder's string in .data
    put32(data(EXPORTS + 20), 4);
    put32(data(EXPORTS + 24), 5);
    put32(data(EXPORTS + 28), EXPORTS + 0x28);
    put32(data(EXPORTS + 32), EXPORTS + 0x40);
    put32(data(EXPORTS + 36), EXPORTS + 0x60);
    U32 name = EXPORTS + 0x80;
    for (U32 i = 0; i < 5; i++) {
        if (i < 4) put32(data(EXPORTS + 0x28 + i * 4), functions[i]);
        put32(data(EXPORTS + 0x40 + i * 4), name);
        put16(data(EXPORTS + 0x60 + i * 2), i); // memset's ordinal 4 is past the 4 functions
        ::memcpy(&f[data(name)], names[i], strlen(names[i]) + 1);
        name += (U32)strlen(names[i]) + 1;
    }
    KProcessPtr process = testContext().process;
    U32 fd = process->memfd_create(B("hostcrt-test"), 0);
    if ((S32)fd < 0) testFail("memfd_create failed");
    std::shared_ptr<KFile> file = std::dynamic_pointer_cast<KFile>(process->getFileDescriptor(fd)->kobject);
    if (file->pwriteNative(f.data(), 0, (U32)f.size()) != (U32)f.size()) testFail("file write failed");
    std::vector<std::pair<U64, U32>> entries = hostCrtParseTest(file.get());
    process->close(fd);
    expect("exports found", (U32)entries.size(), 2);
    if (entries.size() == 2) {
        expect("memcpy offset", (U32)entries[0].first, TEXT + 0x10);
        expect("memcpy is memmove", entries[0].second, hostLibcFnId("memmove"));
        expect("_strnicmp offset", (U32)entries[1].first, TEXT + 0x20);
        expect("_strnicmp", entries[1].second, hostCrtFnId("_strnicmp"));
    }
}

// Catches: a Frida hook (bw_hook_add) under TF raising SIGILL, the hook not firing, or more than the one op running: the audit's
// reading of the head under a trap, on the path testHostLibcDebugTrap's libc head doesn't take (hostHookOp). Serial: hostHooks is a global.
void testHostHookHeadSingleStep() {
    constexpr U32 CODE = TEST_CODE_ADDRESS;
    setup();
#ifdef __EMSCRIPTEN__
    EM_ASM({ globalThis.__hookHits = 0; globalThis.boxedwineFrida = { hit: function() { globalThis.__hookHits++; } }; });
#endif
    KSigAction& ill = testContext().process->sigActions[K_SIGILL];
    KSigAction& trap = testContext().process->sigActions[K_SIGTRAP];
    ill.reset();
    trap.reset();
    ill.handlerAndSigAction = trap.handlerAndSigAction = CODE + 0x40;
    testPushCode8(0x40); // inc eax
    testPushCode8(0x40); // inc eax: must wait for the next step
    EAX = 0;
    ESP = STACK;
    cpu->eip.u32 = CODE;
    U32 id = bw_hook_add(testContext().process->id, CODE);
    cpu->fillFlags();
    cpu->setFlags(TF, TF);
    cpu->nextOp = cpu->getNextOp();
    bool head = cpu->nextOp->inst == Custom1;
    cpu->run();
    bw_hook_remove(id);
    int hits = 1;
#ifdef __EMSCRIPTEN__
    hits = EM_ASM_INT({ var hits = globalThis.__hookHits; delete globalThis.boxedwineFrida; delete globalThis.__hookHits; return hits; });
#endif
    bool illegal = ill.sigInfo[0] == K_SIGILL, trapped = trap.sigInfo[0] == K_SIGTRAP;
    U32 context = trapped ? memory->readd(ESP + 12) : 0;
    U32 eax = trapped ? memory->readd(context + 0x40) : EAX, eip = trapped ? memory->readd(context + 0x4c) : cpu->eip.u32;
    ill.reset();
    trap.reset();
    cpu->debugTrapOnNextInstruction = false;
    cpu->setFlags(0, TF);
    if (!head) testFail("no head op at the hooked address");
    if (illegal) testFail("hooked address under TF raised SIGILL");
    if (!trapped) testFail("TF over a hooked address: no SIGTRAP");
    expect("hook fired once", hits, 1);
    expect("one op ran", eax, 1);
    expect("trap eip", eip, CODE + 1);
}

#endif
