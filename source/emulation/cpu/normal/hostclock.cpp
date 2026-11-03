/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
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
#include "vnboxparam.h"

// Native clock reads: Wine 11.0 has no vDSO-free QueryPerformanceCounter (QpcBypassEnabled is never set, ntdll
// reads no rdtsc, and Boxedwine's rdtsc counts instructions, not time), so every QueryPerformanceCounter /
// timeGetTime (QPC * 1000 / freq) went NtQueryPerformanceCounter's syscall thunk -> __wine_syscall_dispatcher
// (fxsave/fxrstor, stack switch) -> clock_gettime -> a Boxedwine clock_gettime64 syscall. Same for
// GetSystemTimeAsFileTime (NtQuerySystemTime) and GetSystemTimePreciseAsFileTime (RtlGetSystemTimePrecise, a
// unix call). The root fs's ntdll.dll.so (builtin ELF dll) gets a head op (hostLibcOp, the native libc mechanism
// in hosthooks.cpp) on those three exports that does the whole call with the clocks the syscalls read, so the
// values are the ones Wine computed: QPC = CLOCK_BOOTTIME (KSystem::getMicroCounter) in 100 ns ticks, frequency
// 10 MHz; system time = CLOCK_REALTIME(_COARSE) (getSystemTimeAsMicroSeconds) in 100 ns ticks since 1601.
// The two syscall thunks are only taken when their bytes are Wine's i386 thunk with the expected syscall number
// and `ret` (another ntdll build is left to the guest). A pointer that isn't writable falls back to the guest
// (which faults as before). GetTickCount already reads user_shared_data (no syscall). nativeClock=0 turns it off.

enum : U32 { CLOCK_NONE, CLOCK_QPC, CLOCK_SYSTEM_TIME, CLOCK_PRECISE, CLOCK_COUNT };

struct HostClockModule { BString path; U32 offsets[CLOCK_COUNT]; }; // file offset of each export's first byte
static std::vector<HostClockModule> hostClockModules;
static std::vector<BString> hostClockSeen; // ntdll.dll.so paths already looked at

U32 hostElf16(const U8* p); // hosthooks.cpp
U32 hostElf32(const U8* p);
bool hostFileRead(KFile* file, U64 offset, std::vector<U8>& buf, U32 len);

// Wine's i386 syscall thunk: mov eax, id; mov edx, dispatcher; call edx; ret n.
static bool hostClockThunk(const std::vector<U8>& code, U32 id, U32 ret) {
    return code.size() >= 15 && code[0] == 0xb8 && hostElf32(&code[1]) == id && code[5] == 0xba && code[10] == 0xff &&
        code[11] == 0xd2 && code[12] == 0xc2 && hostElf16(&code[13]) == ret;
}

// Fills the export file offsets of a builtin ELF ntdll.dll.so (host-side reads). winebuild puts the PE NT header
// (__wine_spec_nt_header, the RVA base of the export directory and its name tables) in .data; the function table
// holds absolute link-time addresses (relocated by ld.so).
static bool hostClockParse(KFile* file, U32* offsets) {
    std::vector<U8> eh, ph, data, code;
    if (!hostFileRead(file, 0, eh, 52) || memcmp(eh.data(), "\x7f" "ELF\x01\x01", 6) || hostElf16(&eh[18]) != 3) return false; // ELF32 LE i386
    U32 phoff = hostElf32(&eh[28]), phentsize = hostElf16(&eh[42]), phnum = hostElf16(&eh[44]);
    if (phentsize != 32 || !phnum || !hostFileRead(file, phoff, ph, phnum * 32)) return false;
    auto fileOffset = [&](U32 vaddr) -> U32 { // 0 when not in a PT_LOAD's file bytes
        for (U32 i = 0; i < phnum; i++) {
            const U8* p = &ph[i * 32];
            U32 offset = hostElf32(p + 4), va = hostElf32(p + 8), size = hostElf32(p + 16);
            if (hostElf32(p) == 1 && vaddr >= va && vaddr - va < size) return vaddr - va + offset;
        }
        return 0;
    };
    // The NT header is in the writable PT_LOAD (.data); find its signature + i386 machine there.
    U32 nt = 0, ntVaddr = 0;
    for (U32 i = 0; i < phnum && !nt; i++) {
        const U8* p = &ph[i * 32];
        if (hostElf32(p) != 1 || !(hostElf32(p + 24) & 2) || !hostFileRead(file, hostElf32(p + 4), data, hostElf32(p + 16))) continue;
        for (U32 o = 0; o + 24 + 96 + 8 <= data.size(); o += 4) {
            if (!memcmp(&data[o], "PE\0\0\x4c\x01", 6) && hostElf16(&data[o + 24]) == 0x10b) {
                nt = hostElf32(p + 4) + o;
                ntVaddr = hostElf32(p + 8) + o;
                break;
            }
        }
    }
    std::vector<U8> ntHeader, dir, names, ordinals, functions;
    if (!nt || !hostFileRead(file, nt, ntHeader, 24 + 96 + 8)) return false;
    U32 dirOffset = fileOffset(ntVaddr + hostElf32(&ntHeader[24 + 96]));
    if (!dirOffset || !hostFileRead(file, dirOffset, dir, 40)) return false;
    U32 count = hostElf32(&dir[24]), functionCount = hostElf32(&dir[20]);
    U32 namesOffset = fileOffset(ntVaddr + hostElf32(&dir[32])), ordinalsOffset = fileOffset(ntVaddr + hostElf32(&dir[36]));
    U32 functionsOffset = fileOffset(ntVaddr + hostElf32(&dir[28]));
    if (!count || count > 0x10000 || functionCount > 0x10000 || !namesOffset || !ordinalsOffset || !functionsOffset ||
        !hostFileRead(file, namesOffset, names, count * 4) || !hostFileRead(file, ordinalsOffset, ordinals, count * 2) ||
        !hostFileRead(file, functionsOffset, functions, functionCount * 4)) return false;
    static const char* const wanted[CLOCK_COUNT] = { "", "NtQueryPerformanceCounter", "NtQuerySystemTime", "RtlGetSystemTimePrecise" };
    U32 found = 0;
    for (U32 i = 0; i < count; i++) {
        U32 nameOffset = fileOffset(ntVaddr + hostElf32(&names[i * 4]));
        std::vector<U8> name;
        if (!nameOffset || !hostFileRead(file, nameOffset, name, 32)) continue; // all wanted names are shorter, a short read is another name
        for (U32 fn = 1; fn < CLOCK_COUNT; fn++) {
            size_t len = strlen(wanted[fn]) + 1;
            if (offsets[fn] || memcmp(name.data(), wanted[fn], len)) continue;
            U32 ordinal = hostElf16(&ordinals[i * 2]);
            if (ordinal >= functionCount) return false;
            offsets[fn] = fileOffset(hostElf32(&functions[ordinal * 4]));
            if (offsets[fn]) found++;
        }
    }
    if (found != CLOCK_COUNT - 1) return false;
    // NtQueryPerformanceCounter(counter, frequency) is syscall 0x31, NtQuerySystemTime(time) 0x5a in Wine 11.0.
    if (!hostFileRead(file, offsets[CLOCK_QPC], code, 15) || !hostClockThunk(code, 0x31, 8)) return false;
    if (!hostFileRead(file, offsets[CLOCK_SYSTEM_TIME], code, 15) || !hostClockThunk(code, 0x5a, 4)) return false;
    return true;
}

// Called for every file mapping (hosthooks.cpp); true with the export file offsets (CLOCK_COUNT entries, [0]
// unused) the first time a usable ntdll.dll.so is mapped.
bool hostClockMapped(KFile* file, U32* offsets) {
    const BString& path = file->openFile->node->path;
    if (!path.endsWith("/ntdll.dll.so")) return false;
    for (const BString& seen : hostClockSeen) {
        if (seen == path) return false;
    }
    hostClockSeen.push_back(path);
#ifdef __EMSCRIPTEN__
    if (vnboxUrlIs("nativeClock", "0")) {
        klog_fmt("[vnbox] native clock off (nativeClock=0)");
        return false;
    }
#endif
    HostClockModule m = { path, {} };
    if (!hostClockParse(file, m.offsets)) {
        klog_fmt("[vnbox] native clock: %s is not the known Wine 11 ntdll.dll.so layout, left to the guest", path.c_str());
        return false;
    }
    hostClockModules.push_back(m);
    for (U32 i = 0; i < CLOCK_COUNT; i++) offsets[i] = m.offsets[i];
    klog_fmt("[vnbox] native clock: %s, QPC/SystemTime/Precise at file offsets 0x%x/0x%x/0x%x", path.c_str(),
        m.offsets[CLOCK_QPC], m.offsets[CLOCK_SYSTEM_TIME], m.offsets[CLOCK_PRECISE]);
    return true;
}

// CLOCK_* for the export at fileOffset of path, else CLOCK_NONE.
U32 hostClockAt(const BString& path, U32 fileOffset) {
    for (const HostClockModule& m : hostClockModules) {
        if (m.path != path) continue;
        for (U32 fn = 1; fn < CLOCK_COUNT; fn++) {
            if (m.offsets[fn] == fileOffset) return fn;
        }
    }
    return CLOCK_NONE;
}

static const U64 TICKS_1601_TO_1970 = 116444736000000000ull; // Wine's ticks_from_time_t(0)

// The call `fn` at its first instruction (stdcall: return address at ESP, arguments above it). True when done
// here (EAX/EDX set, returned like the function's `ret n`); false (nothing written) runs the guest function.
bool hostClockRun(CPU* cpu, U32 fn) {
    KMemory* memory = cpu->memory;
    if (cpu->debugTrapActive || !cpu->isBig() || cpu->stackMask != 0xFFFFFFFF || cpu->seg[DS].address ||
        cpu->seg[SS].address || !memory->canRead(ESP, 12)) return false;
    U32 a0 = memory->readd(ESP + 4), a1 = memory->readd(ESP + 8), pop = 0;
    switch (fn) {
    case CLOCK_QPC: // NtQueryPerformanceCounter(LARGE_INTEGER* counter, LARGE_INTEGER* frequency): clock_gettime(CLOCK_BOOTTIME)
        if (!memory->canWrite(a0, 8) || (a1 && !memory->canWrite(a1, 8))) return false;
        memory->writeq(a0, KSystem::getMicroCounter() * 10);
        if (a1) memory->writeq(a1, 10000000);
        EAX = 0; // STATUS_SUCCESS
        pop = 8;
        break;
    case CLOCK_SYSTEM_TIME: // NtQuerySystemTime(LARGE_INTEGER* time): clock_gettime(CLOCK_REALTIME_COARSE)
        if (!memory->canWrite(a0, 8)) return false;
        memory->writeq(a0, KSystem::getSystemTimeAsMicroSeconds() * 10 + TICKS_1601_TO_1970);
        EAX = 0;
        pop = 4;
        break;
    case CLOCK_PRECISE: { // LONGLONG RtlGetSystemTimePrecise(void): clock_gettime(CLOCK_REALTIME)
        U64 t = KSystem::getSystemTimeAsMicroSeconds() * 10 + TICKS_1601_TO_1970;
        EAX = (U32)t;
        EDX = (U32)(t >> 32);
        break;
    }
    default:
        return false;
    }
    cpu->eip.u32 = cpu->pop32();
    ESP += pop;
    return true;
}
