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
