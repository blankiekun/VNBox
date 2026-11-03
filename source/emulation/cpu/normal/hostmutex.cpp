/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "boxedwine.h"
#include "vnboxparam.h"

// Native mutexes: the uncontended paths of the root fs glibc 2.38's pthread_mutex_lock and pthread_mutex_unlock
// run as host code (on hostLibcOp, like the native libc string functions in hosthooks.cpp). Wine's unix side takes a pthread mutex around every font, GDI handle and DC access; in
// guest code each lock/unlock pair is ~60 instructions in 15 blocks (kind dispatch, PIC thunk, inline lll_lock,
// __pthread_mutex_unlock_usercnt), not the lock cmpxchg itself.
//
// Same lock word semantics as glibc (nptl/pthread_mutex_lock.c, pthread_mutex_unlock.c): __lock 0/1, __count
// for recursive mutexes, __owner = the thread's tid (THREAD_SELF->tid, %gs:0x68), __nusers. Only the four plain
// kinds (normal, recursive, errorcheck, adaptive; not process-shared, no elision flags) are hosted, and only
// when the mutex is free (lock) or held by the caller (recursive relock, errorcheck EDEADLK/EPERM, unlock with
// no waiters). Everything else (contended lock, unlock with waiters to wake, robust, PI, PP, pshared, elision,
// glibc's owner assertions) is the guest function, from its first instruction, with nothing written.
// Guest threads run one at a time on one host thread (not BOXEDWINE_MULTI_THREADED), so the host op is atomic.
// The offsets below are those of one exact libc build; any other libc.so.6 (code bytes hashed) stays guest.
// nativeMutex=0 turns it off. Chromium, lock+unlock pair: guest 190 ns (recursive 240), host 140 ns; what is left
// is leaving the JIT for the head op, ~65 ns a call. So pthread_getspecific (as hot, ~10 guest instructions, 64 ns)
// is left guest: hosted it took 69 ns.

bool libcRange(KMemory* memory, U32 address, U32 len, bool write); // hosthooks.cpp

void hostHookOff(const char* what); // hosthooks.cpp: tells the page a hook pinned to this root fs did not take

namespace {

// glibc 2.38 i386 /lib/libc.so.6 of TinyCore15Wine11.0 (vaddr == file offset in every segment)
constexpr U32 SYM_LOCK = 0x721f5, SYM_UNLOCK = 0x737b3;
constexpr U32 FORCE_ELISION = 0x19391c; // __pthread_force_elision (GOT 0x191ff4 + 0x1928)
struct CodeRange { U32 start, end, fnv; };
constexpr CodeRange CODE[] = {
    { 0x721ae, 0x723e0, 0xae6cc4de }, // lll_lock (inlined helper) + pthread_mutex_lock
    { 0x7369e, 0x737c5, 0x5e365854 }, // __pthread_mutex_unlock_usercnt + pthread_mutex_unlock
};
constexpr U32 M_LOCK = 0, M_COUNT = 4, M_OWNER = 8, M_KIND = 12, M_NUSERS = 16;
constexpr U32 TCB_TID = 0x68;
constexpr U32 EPERM_ = 1, EAGAIN_ = 11, EDEADLK_ = 35;
constexpr U32 KIND_NORMAL = 0, KIND_RECURSIVE = 1, KIND_ERRORCHECK = 2; // 3 = adaptive (glibc's PTHREAD_MUTEX_*_NP)

bool enabled;

U32 fnv(const U8* p, U32 n) {
    U32 h = 0x811c9dc5;
    for (U32 i = 0; i < n; i++) h = (h ^ p[i]) * 0x01000193;
    return h;
}

bool rd(KMemory* memory, U32 address, U32& value) {
    if (!libcRange(memory, address, 4, false)) return false;
    value = memory->readd(address);
    return true;
}

}

// From hostLibcParse for each FUNC export it wraps: false drops the mutex ones when they can't be hosted.
bool hostMutexWanted(U32 symValue, U32 index) {
    static const U32 expected[] = { SYM_LOCK, SYM_UNLOCK };
    return enabled && symValue == expected[index];
}

// Called once, when the first libc.so.6 is mapped (before its exports are parsed).
void hostMutexCheck(KFile* file) {
#ifdef BOXEDWINE_MULTI_THREADED
    (void)file;
    return; // guest threads run at the same time: the plain reads and writes here would race
#else
#ifdef __EMSCRIPTEN__
    if (vnboxUrlIs("nativeMutex", "0")) {
        klog_fmt("[vnbox] native mutex off (nativeMutex=0)");
        return;
    }
#endif
    std::vector<U8> buf;
    for (const CodeRange& r : CODE) {
        buf.resize(r.end - r.start);
        if (file->preadNative(buf.data(), r.start, (U32)buf.size()) != (S64)buf.size() || fnv(buf.data(), (U32)buf.size()) != r.fnv) {
            klog_fmt("[vnbox] native mutex: libc.so.6 is not the known build, mutexes stay guest");
            hostHookOff("native mutex");
            return;
        }
    }
    enabled = true;
#endif
}

// fn: 0 lock, 1 unlock; the CPU is at the function's entry (flat segments, checked by the caller).
// Sets EAX like the function's `ret`; false (nothing written) = the guest function runs.
bool hostMutexRun(CPU* cpu, U32 fn, U32 a0, U32& result) {
    KMemory* memory = cpu->memory;
    U32 base = cpu->eip.u32 - (fn == 0 ? SYM_LOCK : SYM_UNLOCK); // libc's load address
    U32 gs = cpu->seg[GS].address;
    U32 m = a0, tid;
    if (!libcRange(memory, m, 20, true) || !rd(memory, gs + TCB_TID, tid)) return false;
    U32 kind = memory->readd(m + M_KIND), lock = memory->readd(m + M_LOCK), owner = memory->readd(m + M_OWNER);
    if (kind > 3) return false; // pshared, robust, PI, PP, elision flags: guest
    if (fn == 0) { // pthread_mutex_lock
        if (kind == KIND_RECURSIVE && owner == tid) {
            U32 count = memory->readd(m + M_COUNT);
            if (count == 0xFFFFFFFF) {
                result = EAGAIN_;
                return true;
            }
            memory->writed(m + M_COUNT, count + 1);
            result = 0;
            return true;
        }
        if (kind == KIND_ERRORCHECK && owner == tid) {
            result = EDEADLK_;
            return true;
        }
        if (kind == KIND_NORMAL) {
            U32 forceElision;
            if (!rd(memory, base + FORCE_ELISION, forceElision) || forceElision) return false; // guest sets the elision flag
        }
        if (lock || owner) return false; // contended (futex wait), or glibc's owner assertion
        memory->writed(m + M_LOCK, 1);
        if (kind == KIND_RECURSIVE) memory->writed(m + M_COUNT, 1);
    } else { // pthread_mutex_unlock = __pthread_mutex_unlock_usercnt(m, 1)
        if ((kind == KIND_RECURSIVE || kind == KIND_ERRORCHECK) && owner != tid) {
            result = EPERM_;
            return true;
        }
        if (kind == KIND_ERRORCHECK && !lock) {
            result = EPERM_;
            return true;
        }
        U32 count = memory->readd(m + M_COUNT);
        if (kind == KIND_RECURSIVE && count - 1) {
            memory->writed(m + M_COUNT, count - 1);
            result = 0;
            return true;
        }
        if (lock > 1) return false; // waiters: the guest's lll_unlock wakes one
        if (kind == KIND_RECURSIVE) memory->writed(m + M_COUNT, 0);
        memory->writed(m + M_LOCK, 0);
        tid = 0; // owner
    }
    memory->writed(m + M_OWNER, tid);
    memory->writed(m + M_NUSERS, memory->readd(m + M_NUSERS) + (fn == 0 ? 1 : (U32)-1));
    memory->checkDebugTrapOnMemoryWrite(m, 20);
    result = 0;
    return true;
}
