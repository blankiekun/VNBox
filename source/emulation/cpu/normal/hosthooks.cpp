/*
 *  Copyright (C) 2012-2025  The BoxedWine Team
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

// Host-side hooks for the Frida-compat layer (project/emscripten/frida-compat.js).
// The instruction at a hooked guest address is decoded as its own one-op block
// whose head (inst Custom1, so normalDispatch calls pfn) calls JS with the guest
// paused and then runs the real instruction. Blocks end before a hooked address
// (CPU::shouldContinue), so neither the interpreter nor the wasm JIT can run
// through it; the head is marked NO_JIT. Hooks are per process (the same address
// in Wine's own exes is not the game's code).
//
// JS may only read memory that is already RAM: a not-yet-loaded file page loads
// through the Emscripten FS, which in the JSPI build may suspend, and wasm can't
// suspend under JS frames. So the layer asks for file pages it needs (module
// headers, the main module) to be loaded at attach time, from a syscall where
// the stack is wasm only.
#include "../../softmmu/kmemory_soft.h"
#include "../../softmmu/soft_mmu.h"
#include "../../softmmu/soft_ram.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

struct HostHook { U32 id; U32 pid; U32 eip; };
static std::vector<HostHook> hostHooks;
static std::vector<HostHook> hostHookChanges; // added or removed, code at eip not yet invalidated
static std::vector<std::pair<U32, U32>> hostHookPrefetch; // [address, size) to load before attach
static U32 nextHostHookId = 1;

static bool findHostHook(U32 pid, U32 eip, U32* id) {
    for (const HostHook& h : hostHooks) {
        if (h.eip == eip && h.pid == pid) {
            if (id) *id = h.id;
            return true;
        }
    }
    return false;
}

bool hostHookAt(CPU* cpu, U32 eip) {
    return !hostHooks.empty() && findHostHook(cpu->thread->process->id, eip, nullptr);
}

static KMemory* hostHookMemory(U32 pid) {
    KProcessPtr process = KSystem::getProcess(pid);
    return process ? process->memory : nullptr;
}

// Guest page as host RAM, or null when unreadable or not loaded (never loads anything).
static const U8* hostHookPage(KMemory* memory, U32 page) {
    if (page >= K_NUMBER_OF_PAGES) return nullptr;
    MMU& mmu = getMemData(memory)->mmu[page];
    return mmu.canReadRam ? ramPageGet((RamPage)mmu.ramIndex) : nullptr;
}

static thread_local CPU* hookCpu; // the CPU that hit the hook, while JS runs

// Copies the readable prefix of guest [addr, addr + len) into buf, returns its length.
extern "C" EMSCRIPTEN_KEEPALIVE U32 bw_read(U32 pid, U32 addr, U8* buf, U32 len) {
    KMemory* memory = hostHookMemory(pid);
    U32 n = 0;
    while (memory && n < len) {
        U32 a = addr + n;
        if (a < addr) break; // wrapped
        const U8* page = hostHookPage(memory, a >> K_PAGE_SHIFT);
        if (!page) break;
        U32 chunk = std::min(len - n, K_PAGE_SIZE - (a & K_PAGE_MASK));
        ::memcpy(buf + n, page + (a & K_PAGE_MASK), chunk);
        n += chunk;
    }
    return n;
}

// Frida's Memory.scanSync: addresses in [addr, addr + size) where (byte & mask[i]) == pat[i]
// (pat already masked). Unreadable pages never match. Stores up to maxOut, returns the count.
extern "C" EMSCRIPTEN_KEEPALIVE U32 bw_scan(U32 pid, U32 addr, U32 size, const U8* pat, const U8* mask, U32 patLen, U32* out, U32 maxOut) {
    KMemory* memory = hostHookMemory(pid);
    if (!memory || !patLen) return 0;
    U64 end = std::min((U64)addr + size, (U64)0x100000000ull);
    U32 count = 0;
    U32 cachedPage = 0xFFFFFFFF;
    const U8* cached = nullptr;
    auto byteAt = [&](U32 a, bool& ok) -> U8 {
        if ((a >> K_PAGE_SHIFT) != cachedPage) {
            cachedPage = a >> K_PAGE_SHIFT;
            cached = hostHookPage(memory, cachedPage);
        }
        ok = cached != nullptr;
        return ok ? cached[a & K_PAGE_MASK] : 0;
    };
    for (U64 a = addr; a + patLen <= end; a++) {
        U32 i = 0;
        bool ok = true;
        for (; i < patLen; i++) {
            U8 b = byteAt((U32)(a + i), ok);
            if (!ok || (b & mask[i]) != pat[i]) break;
        }
        if (i == patLen) {
            if (count < maxOut) out[count] = (U32)a;
            count++;
        } else if (!ok && i == 0) {
            a = ((a >> K_PAGE_SHIFT) + 1) * K_PAGE_SIZE - 1; // skip the unreadable page
        }
    }
    return count;
}

// Registers of the CPU that hit the hook: 0-7 eax ecx edx ebx esp ebp esi edi, 8 eip.
extern "C" EMSCRIPTEN_KEEPALIVE U32 bw_reg(U32 i) {
    if (!hookCpu || i > 8) return 0;
    return i == 8 ? hookCpu->seg[CS].address + hookCpu->eip.u32 : hookCpu->reg[i].u32;
}

// Writes one of the general registers (0-7); eip can't be changed from a hook.
extern "C" EMSCRIPTEN_KEEPALIVE void bw_set_reg(U32 i, U32 value) {
    if (hookCpu && i < 8) hookCpu->reg[i].u32 = value;
}

extern "C" EMSCRIPTEN_KEEPALIVE U32 bw_hook_add(U32 pid, U32 eip) {
    HostHook h = { nextHostHookId++, pid, eip };
    hostHooks.push_back(h);
    hostHookChanges.push_back(h);
    return h.id;
}

extern "C" EMSCRIPTEN_KEEPALIVE void bw_hook_remove(U32 id) {
    for (auto it = hostHooks.begin(); it != hostHooks.end(); ++it) {
        if (it->id == id) {
            hostHookChanges.push_back(*it);
            hostHooks.erase(it);
            return;
        }
    }
}

// Asks for the file pages in [addr, addr + size) to be loaded before the next poll or attach.
extern "C" EMSCRIPTEN_KEEPALIVE void bw_prefetch(U32 addr, U32 size) {
    hostHookPrefetch.push_back({ addr, size });
}

#ifdef __EMSCRIPTEN__
// window.boxedwineFrida (frida-compat.js) gets the exports on its first poll.
// 0: nothing to do, 1: load the prefetch ranges then attach, 2: load them then poll again.
EM_JS(int, boxedwine_frida_poll, (U32 pid, U32 tid, U32 teb), {
    var f = globalThis.boxedwineFrida;
    if (!f) return 0;
    if (!f.api) f.api = {read: _bw_read, scan: _bw_scan, reg: _bw_reg, setReg: _bw_set_reg, hookAdd: _bw_hook_add, hookRemove: _bw_hook_remove, prefetch: _bw_prefetch, malloc: _malloc, free: _free, heap: function() { return HEAPU8; }};
    return f.poll(pid, tid, teb);
});
EM_JS(void, boxedwine_frida_attach, (U32 pid, U32 tid, U32 teb), {
    globalThis.boxedwineFrida.attach(pid, tid, teb);
});
EM_JS(void, boxedwine_frida_hit, (U32 id, U32 pid, U32 tid), {
    globalThis.boxedwineFrida.hit(id, pid, tid);
});
#endif

// Called at the start of every syscall, on the guest thread: the stack below is wasm only,
// so loading file pages (which may suspend on a lazy game file) is safe here.
void hostHookSyscall(KThread* thread) {
#ifdef __EMSCRIPTEN__
    U32 pid = thread->process->id;
    for (size_t i = 0; i < hostHookChanges.size();) {
        if (hostHookChanges[i].pid == pid) {
            thread->memory->removeCode(thread, hostHookChanges[i].eip, 1, false);
            hostHookChanges.erase(hostHookChanges.begin() + i);
        } else {
            i++;
        }
    }
    static U32 count;
    if (++count % 1024) return;
    U32 teb = thread->cpu->seg[FS].address;
    for (int round = 0; round < 4; round++) {
        int r = boxedwine_frida_poll(pid, thread->id, teb);
        for (auto& range : hostHookPrefetch) {
            U32 first = range.first >> K_PAGE_SHIFT;
            U32 last = (U32)(std::min((U64)range.first + range.second, (U64)0x100000000ull) - 1) >> K_PAGE_SHIFT;
            for (U32 page = first; range.second && page <= last && page < K_NUMBER_OF_PAGES; page++) {
                MMU& mmu = getMemData(thread->memory)->mmu[page];
                if (mmu.getPageType() == PageType::File && thread->memory->canRead(page)) {
                    thread->memory->readb(page << K_PAGE_SHIFT); // loads the page
                }
            }
        }
        hostHookPrefetch.clear();
        if (r == 1) {
            boxedwine_frida_attach(pid, thread->id, teb);
            hostHookSyscall(thread); // invalidate what the script hooked
        }
        if (r != 2) return;
    }
#endif
}

// Runs the JS hook registered at the CPU's eip, if it is still there (called by hostHookOp).
void hostHookHit(CPU* cpu) {
    U32 id;
    if (findHostHook(cpu->thread->process->id, cpu->seg[CS].address + cpu->eip.u32, &id)) {
#ifdef __EMSCRIPTEN__
        hookCpu = cpu;
        boxedwine_frida_hit(id, cpu->thread->process->id, cpu->thread->id);
        hookCpu = nullptr;
#endif
    }
}

// Native libc ("wrapped libs", Box64's trick): the hot string/memory exports of the root fs libc (glibc
// 2.38 /lib/libc.so.6, plain FUNC symbols, no IFUNCs) run as host C++ instead of guest code. getOp gives a
// block starting at one of them a head op (hostLibcOp, normalCPU.cpp) that does the whole call, sets EAX
// and returns like the function's `ret`. Whatever the host side can't match exactly falls back to the
// guest function, which then runs (and faults) as it always did: a page that isn't plain RAM (unmapped,
// read-only, copy-on-write, code, not loaded yet), DF set, debug watchpoints, non-flat segments, errno
// cases (strtol overflow, bad base). Nothing is written before the checks pass. nativeLibc=0 turns it off.
// shortcut: strtol/strtoul use C-locale ctype (ASCII), which every glibc locale matches except Turkish ones.
enum : U32 {
    LIBC_NONE, LIBC_MEMCPY, LIBC_MEMMOVE, LIBC_MEMSET, LIBC_MEMCMP, LIBC_MEMCHR, LIBC_STRLEN, LIBC_STRNLEN,
    LIBC_STRCMP, LIBC_STRNCMP, LIBC_STRCHR, LIBC_STRRCHR, LIBC_STRCPY, LIBC_STRTOL, LIBC_STRTOUL,
    LIBC_STRTOL23, LIBC_STRTOUL23, LIBC_COUNT,
    HOST_ACM = LIBC_COUNT // vorbis.acm's DriverProc (hostacm.cpp), on the same head op
};
static const char* const hostLibcNames[LIBC_COUNT] = {
    "", "memcpy", "memmove", "memset", "memcmp", "memchr", "strlen", "strnlen", "strcmp", "strncmp",
    "strchr", "strrchr", "strcpy", "strtol", "strtoul", "__isoc23_strtol", "__isoc23_strtoul"
};
static std::atomic<int> hostLibcState; // 0: no libc mapped yet, 1: table ready (set after the table), -1: off or unusable
static BString hostLibcPath;
static std::vector<std::pair<U64, U32>> hostLibcEntries; // file offset of an export, LIBC_*
static U8 hostLibcPageOffsets[K_PAGE_SIZE / 8]; // bit per page offset of an entry: getOp's cheap filter

U32 hostLibcFnId(const char* name) {
    for (U32 i = 1; i < LIBC_COUNT; i++) {
        if (!strcmp(hostLibcNames[i], name)) return i;
    }
    return LIBC_NONE;
}

static U32 elf16(const U8* p) { return p[0] | (p[1] << 8); }
static U32 elf32(const U8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((U32)p[3] << 24); }

// Reads the exports from the file's section headers and .dynsym (host-side reads, the guest isn't involved).
static bool hostLibcParse(KFile* file) {
    auto read = [file](U64 offset, std::vector<U8>& buf, U32 len) {
        buf.resize(len);
        return file->preadNative(buf.data(), (S64)offset, len) == len;
    };
    std::vector<U8> eh, sh, syms, names;
    if (!read(0, eh, 52) || memcmp(eh.data(), "\x7f" "ELF\x01\x01", 6) || elf16(&eh[18]) != 3) return false; // ELF32 LE i386
    U32 shoff = elf32(&eh[32]), shentsize = elf16(&eh[46]), shnum = elf16(&eh[48]);
    if (shentsize != 40 || !shnum || !read(shoff, sh, shnum * 40)) return false;
    for (U32 i = 0; i < shnum; i++) {
        const U8* s = &sh[i * 40];
        U32 link = elf32(s + 24);
        if (elf32(s + 4) != 11 || link >= shnum) continue; // SHT_DYNSYM
        const U8* str = &sh[link * 40];
        if (!read(elf32(s + 16), syms, elf32(s + 20)) || !read(elf32(str + 16), names, elf32(str + 20))) return false;
        for (U32 off = 0; off + 16 <= syms.size(); off += 16) {
            const U8* sym = &syms[off];
            U32 name = elf32(sym), value = elf32(sym + 4), shndx = elf16(sym + 14);
            if ((sym[12] & 0xf) != 2 || !shndx || shndx >= shnum || name >= names.size()) continue; // STT_FUNC only (an IFUNC is a resolver)
            if (!memchr(&names[name], 0, names.size() - name)) continue;
            U32 fn = hostLibcFnId((const char*)&names[name]);
            if (!fn) continue;
            const U8* sec = &sh[shndx * 40];
            U64 fileOffset = (U64)value - elf32(sec + 12) + elf32(sec + 16); // st_value - sh_addr + sh_offset
            hostLibcEntries.push_back({ fileOffset, fn });
            hostLibcPageOffsets[(fileOffset & K_PAGE_MASK) >> 3] |= 1 << (fileOffset & 7);
        }
        return !hostLibcEntries.empty();
    }
    return false;
}

bool hostAcmMapped(KFile* file, U32& driverProc); // hostacm.cpp
bool hostAcmAt(const BString& path, U32 fileOffset);
bool hostAcmRun(CPU* cpu);
static std::atomic<bool> hostAcmAny;

// Called by KMemory::mmap for every file mapping; the first libc.so.6 mapped loads the table, a known
// vorbis.acm adds its DriverProc.
void hostLibcMapped(KFile* file) {
    if (!file || !file->openFile || !file->openFile->node) return;
    U32 driverProc;
    if (hostAcmMapped(file, driverProc)) {
        hostLibcPageOffsets[(driverProc & K_PAGE_MASK) >> 3] |= 1 << (driverProc & 7);
        hostAcmAny = true;
        return;
    }
    if (hostLibcState) return;
    const BString& path = file->openFile->node->path;
    if (!path.endsWith("/libc.so.6")) return;
#ifdef __EMSCRIPTEN__
    if (EM_ASM_INT({ return typeof vnboxParam === "function" && vnboxParam("nativeLibc") === "0" ? 1 : 0; })) {
        hostLibcState = -1;
        klog_fmt("[vnbox] native libc off (nativeLibc=0)");
        return;
    }
#endif
    bool ok = hostLibcParse(file);
    if (ok) hostLibcPath = path;
    hostLibcState = ok ? 1 : -1;
    klog_fmt("[vnbox] native libc: %s, %d functions", path.c_str(), (int)hostLibcEntries.size());
}

// LIBC_* (or HOST_ACM) when the block at eip (linear) starts one of the wrapped exports in this process, else LIBC_NONE.
U32 hostLibcAt(CPU* cpu, U32 eip) {
    if ((hostLibcState != 1 && !hostAcmAny) || !(hostLibcPageOffsets[(eip & K_PAGE_MASK) >> 3] & (1 << (eip & 7)))) return LIBC_NONE;
    KProcess* process = cpu->thread->process.get();
    U32 offset = process->getModuleEip(eip); // file offset in the mapping holding eip
    if (!offset) return LIBC_NONE;
    BString name = process->getModuleName(eip);
    if (hostLibcState == 1 && name == hostLibcPath) {
        for (auto& e : hostLibcEntries) {
            if (e.first == offset) return e.second;
        }
        return LIBC_NONE;
    }
    return hostAcmAny && hostAcmAt(name, offset) ? HOST_ACM : LIBC_NONE;
}

// Guest address as host RAM if its page is plain RAM with that access (the soft MMU's own fast-path test).
static U8* libcRam(KMemory* memory, U32 address, bool write) {
    MMU& mmu = getMemData(memory)->mmu[address >> K_PAGE_SHIFT];
    if (write ? !mmu.canWriteRam : !mmu.canReadRam) return nullptr;
    return ramPageGet((RamPage)mmu.ramIndex) + (address & K_PAGE_MASK);
}

static bool libcRange(KMemory* memory, U32 address, U32 len, bool write) {
    if (!len) return true;
    if ((U64)address + len > 0x100000000ull) return false;
    for (U32 page = address >> K_PAGE_SHIFT; page <= (address + len - 1) >> K_PAGE_SHIFT; page++) {
        if (!libcRam(memory, page << K_PAGE_SHIFT, write)) return false;
    }
    return true;
}

// glibc's word-at-a-time code may read up to the end of the aligned word after the last byte it needs
// (always in the same or the next page): that page must be readable too, or the guest could fault there.
static bool libcReadAhead(KMemory* memory, U32 last) {
    U32 end = (last & ~3u) + 7;
    if (end < last) return false;
    return (end >> K_PAGE_SHIFT) == (last >> K_PAGE_SHIFT) || libcRam(memory, end, false);
}

// Index of the first byte of guest [address, address + max) where stop (called per page piece with its guest
// address, returns the index in it or n) stops, or max. False when a page it needs isn't readable RAM.
template <typename Stop>
static bool libcScan(KMemory* memory, U32 address, U32 max, U32& index, Stop stop) {
    for (U32 i = 0; i < max;) {
        U32 a = address + i;
        if (a < address) return false; // wrapped
        const U8* p = libcRam(memory, a, false);
        if (!p) return false;
        U32 n = std::min(max - i, K_PAGE_SIZE - (a & K_PAGE_MASK));
        U32 j = stop(p, n, a);
        if (j < n) {
            index = i + j;
            return libcReadAhead(memory, a + j);
        }
        i += n;
    }
    index = max;
    return !max || libcReadAhead(memory, address + max - 1);
}

static bool libcFindByte(KMemory* memory, U32 address, U32 max, U8 value, U32& index) {
    return libcScan(memory, address, max, index, [value](const U8* p, U32 n, U32) {
        const U8* hit = (const U8*)::memchr(p, value, n);
        return hit ? (U32)(hit - p) : n;
    });
}

// Byte copy in the order of `rep movsb` (forward, or backward with DF set as memmove does), page by page;
// overlapping host ranges, including two guest pages on the same RAM, copy byte by byte in that order.
// The caller checked both ranges.
static void libcCopy(KMemory* memory, U32 dst, U32 src, U32 len, bool backward) {
    while (len) {
        U32 d = backward ? dst + len - 1 : dst, s = backward ? src + len - 1 : src;
        U32 n = backward ? std::min({ len, (d & K_PAGE_MASK) + 1, (s & K_PAGE_MASK) + 1 })
                         : std::min({ len, K_PAGE_SIZE - (d & K_PAGE_MASK), K_PAGE_SIZE - (s & K_PAGE_MASK) });
        U8* to = libcRam(memory, backward ? d - (n - 1) : d, true);
        const U8* from = libcRam(memory, backward ? s - (n - 1) : s, false);
        uintptr_t t = (uintptr_t)to, f = (uintptr_t)from;
        if (t <= f ? f - t >= n || !backward : t - f >= n || backward) {
            ::memmove(to, from, n); // same result as the byte order here
        } else if (backward) {
            for (U32 i = n; i-- > 0;) to[i] = from[i];
        } else {
            for (U32 i = 0; i < n; i++) to[i] = from[i];
        }
        if (!backward) {
            dst += n;
            src += n;
        }
        len -= n;
    }
}

// glibc's strtol family (stdlib/strtol_l.c) for the C locale. False for the errno cases (overflow, bad base).
static bool libcStrtol(KMemory* memory, U32 nptr, U32 endptr, S32 base, bool isSigned, bool binary, U32& result) {
    if (base < 0 || base == 1 || base > 36) return false; // EINVAL
    if (endptr && !libcRange(memory, endptr, 4, true)) return false;
    U32 s = nptr;
    bool ok = true;
    auto at = [&](U32 a) -> U8 {
        const U8* p = libcRam(memory, a, false);
        if (!p) ok = false;
        return p ? *p : 0;
    };
    auto upper = [](U8 c) -> U8 { return c >= 'a' && c <= 'z' ? c - 32 : c; };
    U8 c = at(s);
    while (c == ' ' || (c >= '\t' && c <= '\r')) c = at(++s);
    if (!ok) return false;
    bool negative = false;
    U32 value = 0, save = nptr;
    if (c) {
        if (c == '-' || c == '+') {
            negative = c == '-';
            s++;
        }
        if (at(s) == '0') {
            U8 next = upper(at(s + 1));
            if ((base == 0 || base == 16) && next == 'X') {
                s += 2;
                base = 16;
            } else if ((base == 0 || base == 2) && next == 'B' && binary) {
                s += 2;
                base = 2;
            } else if (base == 0) {
                base = 8;
            }
        } else if (base == 0) {
            base = 10;
        }
        save = s;
        U32 cutoff = 0xFFFFFFFFu / (U32)base, cutlim = 0xFFFFFFFFu % (U32)base;
        bool overflow = false;
        for (c = at(s); c && ok; c = at(++s)) {
            U32 digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (upper(c) >= 'A' && upper(c) <= 'Z') digit = upper(c) - 'A' + 10;
            else break;
            if ((S32)digit >= base) break;
            if (value > cutoff || (value == cutoff && digit > cutlim)) overflow = true;
            else value = value * (U32)base + digit;
        }
        if (!ok || !libcReadAhead(memory, s)) return false;
        if (s != save) {
            if (isSigned && !overflow && value > (negative ? 0x80000000u : 0x7FFFFFFFu)) overflow = true;
            if (overflow) return false; // ERANGE
            if (endptr) memory->writed(endptr, s);
            result = negative ? 0u - value : value;
            return true;
        }
    }
    // noconv: nothing parsed; "0x"/"0b" with no digit after it ends at the x/b
    if (endptr) {
        bool prefix = save - nptr >= 2 && at(save - 2) == '0' && (upper(at(save - 1)) == 'X' || (binary && upper(at(save - 1)) == 'B'));
        if (!ok) return false;
        memory->writed(endptr, prefix ? save - 1 : nptr);
    }
    result = 0;
    return true;
}

// Runs the wrapped function `fn` for the call the CPU is at (i386 cdecl: return address at ESP, arguments
// above it); on success sets EAX and pops the return address into eip, as the function's `ret` would.
// False (nothing changed) means the guest function has to run.
bool hostLibcRun(CPU* cpu, U32 fn) {
    if (fn == HOST_ACM) return hostAcmRun(cpu);
    KMemory* memory = cpu->memory;
    if (cpu->getDirection() < 0 || cpu->debugTrapActive || !cpu->isBig() || cpu->stackMask != 0xFFFFFFFF ||
        cpu->seg[CS].address || cpu->seg[DS].address || cpu->seg[ES].address || cpu->seg[SS].address ||
        !libcRange(memory, ESP, 16, false)) return false; // return address + up to 3 arguments
    U32 a0 = memory->readd(ESP + 4), a1 = memory->readd(ESP + 8), a2 = memory->readd(ESP + 12);
    U32 result = 0, index;
    switch (fn) {
    case LIBC_MEMCPY: // rep movsb
    case LIBC_MEMMOVE: // backward when dst > src, nothing when dst == src
        if (fn == LIBC_MEMMOVE && a0 == a1) break;
        if (!libcRange(memory, a1, a2, false) || !libcRange(memory, a0, a2, true)) return false;
        libcCopy(memory, a0, a1, a2, fn == LIBC_MEMMOVE && a0 > a1);
        memory->checkDebugTrapOnMemoryWrite(a0, a2);
        result = a0;
        break;
    case LIBC_MEMSET: // rep stosb
        if (!libcRange(memory, a0, a2, true)) return false;
        for (U32 done = 0; done < a2;) {
            U32 n = std::min(a2 - done, K_PAGE_SIZE - ((a0 + done) & K_PAGE_MASK));
            ::memset(libcRam(memory, a0 + done, true), (U8)a1, n);
            done += n;
        }
        memory->checkDebugTrapOnMemoryWrite(a0, a2);
        result = a0;
        break;
    case LIBC_MEMCMP: // repz cmpsb: reads up to the first difference only; -1/0/1
        for (U32 done = 0; done < a2 && !result;) {
            U32 x = a0 + done, y = a1 + done;
            const U8* p = x >= a0 ? libcRam(memory, x, false) : nullptr;
            const U8* q = y >= a1 ? libcRam(memory, y, false) : nullptr;
            if (!p || !q) return false;
            U32 n = std::min({ a2 - done, K_PAGE_SIZE - (x & K_PAGE_MASK), K_PAGE_SIZE - (y & K_PAGE_MASK) });
            int r = ::memcmp(p, q, n);
            result = r < 0 ? (U32)-1 : r > 0 ? 1 : 0;
            done += n;
        }
        break;
    case LIBC_MEMCHR:
        if (!libcFindByte(memory, a0, a2, (U8)a1, index)) return false;
        result = index < a2 ? a0 + index : 0;
        break;
    case LIBC_STRLEN:
        if (!libcFindByte(memory, a0, 0xFFFFFFFF, 0, index)) return false;
        result = index;
        break;
    case LIBC_STRNLEN: // memchr(s, 0, n)
        if (!libcFindByte(memory, a0, a1, 0, index)) return false;
        result = index;
        break;
    case LIBC_STRCHR: { // first c, or the terminator when c is 0
        U8 c = (U8)a1;
        if (!libcScan(memory, a0, 0xFFFFFFFF, index, [c](const U8* p, U32 n, U32) {
            for (U32 i = 0; i < n; i++) if (p[i] == c || !p[i]) return i;
            return n;
        })) return false;
        result = memory->readb(a0 + index) == c ? a0 + index : 0;
        break;
    }
    case LIBC_STRRCHR: { // last c before the terminator, or the terminator when c is 0
        U8 c = (U8)a1;
        U32 last = 0;
        if (!libcScan(memory, a0, 0xFFFFFFFF, index, [c, &last](const U8* p, U32 n, U32 a) {
            for (U32 i = 0; i < n; i++) {
                if (!p[i]) return i;
                if (p[i] == c) last = a + i;
            }
            return n;
        })) return false;
        result = c ? last : a0 + index;
        break;
    }
    case LIBC_STRCMP:
    case LIBC_STRNCMP: { // byte difference at the first mismatch or terminator (string/strcmp.c, strncmp.c)
        U32 max = fn == LIBC_STRNCMP ? a2 : 0xFFFFFFFF, i = 0;
        S32 r = 0;
        bool done = false;
        while (!done && i < max) {
            U32 x = a0 + i, y = a1 + i;
            const U8* p = x >= a0 ? libcRam(memory, x, false) : nullptr;
            const U8* q = y >= a1 ? libcRam(memory, y, false) : nullptr;
            if (!p || !q) return false;
            U32 n = std::min({ max - i, K_PAGE_SIZE - (x & K_PAGE_MASK), K_PAGE_SIZE - (y & K_PAGE_MASK) });
            for (U32 j = 0; j < n; j++) {
                if (p[j] != q[j] || !p[j]) {
                    r = (S32)p[j] - (S32)q[j];
                    i += j;
                    done = true;
                    break;
                }
            }
            if (!done) i += n;
        }
        U32 last = done ? i : i - 1; // the last byte compared, if any
        if ((done || i) && (!libcReadAhead(memory, a0 + last) || !libcReadAhead(memory, a1 + last))) return false;
        result = (U32)r;
        break;
    }
    case LIBC_STRCPY: { // returns dst; overlapping strings (undefined in C) are left to the guest
        if (!libcFindByte(memory, a1, 0xFFFFFFFF, 0, index)) return false;
        U32 len = index + 1;
        if (len == 0 || !libcRange(memory, a0, len, true) || (a0 < a1 + len && a1 < a0 + len)) return false;
        libcCopy(memory, a0, a1, len, false);
        memory->checkDebugTrapOnMemoryWrite(a0, len);
        result = a0;
        break;
    }
    case LIBC_STRTOL:
    case LIBC_STRTOUL:
    case LIBC_STRTOL23:
    case LIBC_STRTOUL23:
        if (!libcStrtol(memory, a0, a1, (S32)a2, fn == LIBC_STRTOL || fn == LIBC_STRTOL23, fn >= LIBC_STRTOL23, result)) return false;
        break;
    default:
        return false;
    }
    EAX = result;
    cpu->eip.u32 = cpu->pop32();
    return true;
}
