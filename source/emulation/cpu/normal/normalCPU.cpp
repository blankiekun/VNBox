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

#include "../decoder.h"
#include "normalCPU.h"
#include "../../softmmu/soft_code_page.h"
#include "../../softmmu/kmemory_soft.h"
#include "../armv7/armv7CPU.h"
#include "../armv8/armv8CPU.h"
#include "../../../util/ptrpool.h"
#include "../wasm/wasmMemoryOrdering.h"
#if defined(BOXEDWINE_JIT) && !defined(BOXEDWINE_WASM_JIT)
#include "../jit/jitCodeGen.h"
#endif

#ifdef __TEST
thread_local U32 normalWaitCallCount = 0;
#endif

#if defined(BOXEDWINE_MULTI_THREADED) && defined(BOXEDWINE_WASM_JIT)
static inline void normalOrderMemoryInstruction(DecodedOp* op) {
    // Match the JIT's guest-memory instruction boundaries during warmup and
    // single-op fallback. Ordinary Wasm loads/stores do not provide x86 TSO
    // on weakly ordered hosts, even when another access uses a locked op.
    if (instructionInfo[op->inst].readMemWidth || instructionInfo[op->inst].writeMemWidth) {
        if (wasmJitNeedsMemoryFence) {
            std::atomic_thread_fence(std::memory_order_seq_cst);
        } else {
            // Retain the C++ compiler barrier even without hardware fences.
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }
    }
}
#define START_OP(cpu, op) normalOrderMemoryInstruction(op)
#elif defined(BOXEDWINE_MULTI_THREADED)
#ifdef _DEBUG
//#define START_OP(cpu, op) op->log(cpu)
#define START_OP(cpu, op)
#else
#define START_OP(cpu, op)
#endif
#else
#ifdef _DEBUG
#define START_OP(cpu, op) cpu->blockInstructionCount++; op->log(cpu)
 //#define START_OP(cpu, op)
#else
#define START_OP(cpu, op) cpu->blockInstructionCount++
#endif
#endif

// Direct-dispatch builds use normalDispatch so the switch generates direct
// return_call instructions per opcode rather than one return_call_indirect.
// WASM JIT interpreter chains also use normalDispatch until they reach a JIT
// block head, where they return to the regular run loop.
#if defined(BOXEDWINE_DIRECT_NORMAL_DISPATCH)
#define NEXT() cpu->eip.u32+=op->len; MUSTTAIL return normalDispatch(cpu, op->next);
#elif defined(BOXEDWINE_WASM_JIT)
#ifndef BOXEDWINE_WASM_JIT_NO_DIRECT_INTERP
// Non-bridge interpreter chains go through normalDispatch's switch (direct
// tail calls) instead of the indirect nextOp->pfn call. Default on: A/B
// measured MT 62 -> 82 (beats the 80 interpreter) and ST 27 -> 28 / 30
// with piped modules; BOXEDWINE_WASM_JIT_NO_DIRECT_INTERP is a diagnostic
// opt-out. Chains exit to run() at compiled-block heads so wasmStartJITOp
// keeps its machinery (per-worker readiness, relocBase, runCount counting);
// branches still break chains in NEXT_BRANCH1/2 - that is the stack bound
// and must stay (the historical branch-through variant overflowed the JS
// stack). Any DecodedOp whose pfn is not normalOps[inst] must keep an inst
// that lands in normalDispatch's default: case (see cpu.cpp's lastOp).
#define NEXT_INTERP_CHAIN()                                                                         \
    if (nextOp && nextOp->pfn != cpu->thread->process->startJITOp) {                                \
        MUSTTAIL return normalDispatch(cpu, nextOp);                                                \
    }                                                                                                \
    cpu->nextOp = nextOp;                                                                            \
    return
#else
#define NEXT_INTERP_CHAIN() MUSTTAIL return nextOp->pfn(cpu, nextOp)
#endif
#define NEXT() do {                                                                                 \
    cpu->eip.u32 += op->len;                                                                         \
    DecodedOp* nextOp = op->next ? op->next : cpu->getNextOp();                                      \
    NEXT_INTERP_CHAIN();                                                                             \
} while (0)
#else
#define NEXT() cpu->eip.u32+=op->len; MUSTTAIL return op->next->pfn(cpu, op->next);
#endif
#define NEXT_DONE() cpu->nextOp = cpu->getNextOp();
#define NEXT_DONE_JUMP_OR_CALL() cpu->nextOp = cpu->getNextOp(OP_FLAG2_JUMP_TARGET);

#if defined(BOXEDWINE_DIRECT_NORMAL_DISPATCH) || defined(BOXEDWINE_WASM_JIT)
static inline bool normalGetZF(CPU* cpu) {
    switch (cpu->lazyFlagType) {
    case FLAGS_NONE:
        return (cpu->flags & ZF) != 0;
    case FLAGS_ADD8:
    case FLAGS_OR8:
    case FLAGS_AND8:
    case FLAGS_SUB8:
    case FLAGS_XOR8:
    case FLAGS_INC8:
    case FLAGS_DEC8:
    case FLAGS_CMP8:
    case FLAGS_TEST8:
        return cpu->result.u8 == 0;
    case FLAGS_ADD16:
    case FLAGS_OR16:
    case FLAGS_AND16:
    case FLAGS_SUB16:
    case FLAGS_XOR16:
    case FLAGS_INC16:
    case FLAGS_DEC16:
    case FLAGS_CMP16:
    case FLAGS_TEST16:
        return cpu->result.u16 == 0;
    case FLAGS_ADD32:
    case FLAGS_OR32:
    case FLAGS_AND32:
    case FLAGS_SUB32:
    case FLAGS_XOR32:
    case FLAGS_INC32:
    case FLAGS_DEC32:
    case FLAGS_CMP32:
    case FLAGS_TEST32:
        return cpu->result.u32 == 0;
    default:
        return cpu->getZF();
    }
}

static inline bool normalGetCF(CPU* cpu) {
    switch (cpu->lazyFlagType) {
    case FLAGS_NONE:
        return (cpu->flags & CF) != 0;
    case FLAGS_ADD8:
        return cpu->result.u8 < cpu->dst.u8;
    case FLAGS_ADD16:
        return cpu->result.u16 < cpu->dst.u16;
    case FLAGS_ADD32:
        return cpu->result.u32 < cpu->dst.u32;
    case FLAGS_SUB8:
    case FLAGS_CMP8:
        return cpu->dst.u8 < cpu->src.u8;
    case FLAGS_SUB16:
    case FLAGS_CMP16:
        return cpu->dst.u16 < cpu->src.u16;
    case FLAGS_SUB32:
    case FLAGS_CMP32:
        return cpu->dst.u32 < cpu->src.u32;
    case FLAGS_OR8:
    case FLAGS_OR16:
    case FLAGS_OR32:
    case FLAGS_AND8:
    case FLAGS_AND16:
    case FLAGS_AND32:
    case FLAGS_XOR8:
    case FLAGS_XOR16:
    case FLAGS_XOR32:
    case FLAGS_TEST8:
    case FLAGS_TEST16:
    case FLAGS_TEST32:
        return false;
    case FLAGS_INC8:
    case FLAGS_INC16:
    case FLAGS_INC32:
    case FLAGS_DEC8:
    case FLAGS_DEC16:
    case FLAGS_DEC32:
        return cpu->oldCF != 0;
    default:
        return cpu->getCF();
    }
}

static inline bool normalGetNLE(CPU* cpu) {
    switch (cpu->lazyFlagType) {
    case FLAGS_NONE:
        return !(cpu->flags & ZF) && (((cpu->flags & SF) != 0) == ((cpu->flags & OF) != 0));
    case FLAGS_CMP8:
    case FLAGS_SUB8: {
        U8 result = cpu->result.u8;
        U8 overflow = (cpu->dst.u8 ^ cpu->src.u8) & (cpu->dst.u8 ^ result) & 0x80;
        return result != 0 && ((result & 0x80) != 0) == (overflow != 0);
    }
    case FLAGS_CMP16:
    case FLAGS_SUB16: {
        U16 result = cpu->result.u16;
        U16 overflow = (cpu->dst.u16 ^ cpu->src.u16) & (cpu->dst.u16 ^ result) & 0x8000;
        return result != 0 && ((result & 0x8000) != 0) == (overflow != 0);
    }
    case FLAGS_CMP32:
    case FLAGS_SUB32: {
        U32 result = cpu->result.u32;
        U32 overflow = (cpu->dst.u32 ^ cpu->src.u32) & (cpu->dst.u32 ^ result) & 0x80000000;
        return result != 0 && ((result & 0x80000000) != 0) == (overflow != 0);
    }
    case FLAGS_OR8:
    case FLAGS_AND8:
    case FLAGS_XOR8:
    case FLAGS_TEST8:
        return cpu->result.u8 != 0 && (cpu->result.u8 & 0x80) == 0;
    case FLAGS_OR16:
    case FLAGS_AND16:
    case FLAGS_XOR16:
    case FLAGS_TEST16:
        return cpu->result.u16 != 0 && (cpu->result.u16 & 0x8000) == 0;
    case FLAGS_OR32:
    case FLAGS_AND32:
    case FLAGS_XOR32:
    case FLAGS_TEST32:
        return cpu->result.u32 != 0 && (cpu->result.u32 & 0x80000000) == 0;
    default:
        return !cpu->getZF() && cpu->getSF() == cpu->getOF();
    }
}
#else
#define normalGetZF(cpu) ((cpu)->getZF())
#define normalGetCF(cpu) ((cpu)->getCF())
#define normalGetNLE(cpu) (!(cpu)->getZF() && (cpu)->getSF() == (cpu)->getOF())
#endif

// if jmp back, then return so that we don't blow the stack
#define NEXT_BRANCH1()                                      \
    cpu->eip.u32+=op->len;                                  \
    if (!(*(op->data.nextJump))) {                          \
        *(op->data.nextJump) = cpu->getNextOp(OP_FLAG2_JUMP_TARGET);            \
    }                                                       \
    cpu->nextOp = *(op->data.nextJump);

#define NEXT_BRANCH2() cpu->eip.u32+=op->len; if (!op->next) {op->next = cpu->getNextOp(); } cpu->nextOp = op->next;

#if defined(BOXEDWINE_DIRECT_NORMAL_DISPATCH) || defined(BOXEDWINE_WASM_JIT)
static void OPCALL normalDispatch(CPU* cpu, DecodedOp* op);
#endif

#include "instructions.h"
#include "normal_arith.h"
#include "normal_conditions.h"
#include "normal_incdec.h"
#include "normal_pushpop.h"
#include "normal_setcc.h"
#include "normal_strings.h"
#include "normal_strings_op.h"
#include "normal_shift.h"
#include "normal_shift_op.h"
#include "normal_xchg.h"
#include "normal_bit.h"
#include "normal_mmx.h"
#include "normal_sse.h"
#include "normal_sse2.h"
#include "normal_fpu.h"
#include "normal_other.h"
#include "normal_jump.h"
#include "normal_move.h"

#include "normal_lock.h"

static OpCallback normalOps[NUMBER_OF_OPS];
static U32 normalOpsInitialized;

void OPCALL normal_sgdt(CPU* cpu, DecodedOp* op) {
    START_OP(cpu, op);
    U32 address = eaa(cpu, op);
    // Descriptor tables live in host data structures, not guest memory. Like
    // SIDT, expose a stable synthetic descriptor for user-mode inspection.
    cpu->memory->writew(address, LDT_ENTRIES * 8 - 1);
    cpu->memory->writed(address + 2, 0);
    NEXT();
}

void OPCALL normal_sidt(CPU* cpu, DecodedOp* op) {
    START_OP(cpu, op);    
    U32 eaa = eaa(cpu, op);
    cpu->memory->writew(eaa, 1023); // limit
    cpu->memory->writed(eaa + 2, 0); // base
    NEXT();
}

void OPCALL normal_sldt_reg(CPU* cpu, DecodedOp* op) {
    START_OP(cpu, op);
    // There is no hardware LDTR in the user-mode emulator.
    if (op->imm == 32) cpu->reg[op->reg].u32 = 0;
    else cpu->reg[op->reg].u16 = 0;
    NEXT();
}

void OPCALL normal_sldt_e16(CPU* cpu, DecodedOp* op) {
    START_OP(cpu, op);
    cpu->memory->writew(eaa(cpu, op), 0);
    NEXT();
}

void OPCALL normal_str_reg(CPU* cpu, DecodedOp* op) {
    START_OP(cpu, op);
    if (op->imm == 32) cpu->reg[op->reg].u32 = 0;
    else cpu->reg[op->reg].u16 = 0;
    NEXT();
}

void OPCALL normal_str_e16(CPU* cpu, DecodedOp* op) {
    START_OP(cpu, op);
    cpu->memory->writew(eaa(cpu, op), 0);
    NEXT();
}

void OPCALL normal_smsw_reg(CPU* cpu, DecodedOp* op) {
    START_OP(cpu, op);
    cpu->reg[op->reg].u16 = 0;
    NEXT();
}

void OPCALL normal_smsw_e16(CPU* cpu, DecodedOp* op) {
    START_OP(cpu, op);
    cpu->memory->writew(eaa(cpu, op), 0);
    NEXT();
}

void OPCALL onTestEnd(CPU* cpu, DecodedOp* op) {
    cpu->nextOp = op;
}

// Dispatch the next decoded op by switching on its instruction ID. Using a
// switch lets WASM emit direct return_call instructions for opcode arms.
#if defined(BOXEDWINE_DIRECT_NORMAL_DISPATCH) || defined(BOXEDWINE_WASM_JIT)
static void OPCALL normalDispatch(CPU* cpu, DecodedOp* op) {
    switch (op->inst) {
#undef INIT_CPU
#define INIT_CPU(e, f) case e: MUSTTAIL return normal_##f(cpu, op);
#include "../common/cpu_init.h"
#include "../common/cpu_init_mmx.h"
#include "../common/cpu_init_sse.h"
#include "../common/cpu_init_sse2.h"
#include "../common/cpu_init_fpu.h"
#undef INIT_CPU
#ifdef BOXEDWINE_MULTI_THREADED
#define INIT_CPU_LOCK(e, f) case e##_Lock: MUSTTAIL return normal_##f##_lock(cpu, op);
#include "../common/cpu_init_lock.h"
#undef INIT_CPU_LOCK
#endif
        case SLDTReg: MUSTTAIL return normal_sldt_reg(cpu, op);
        case SLDTE16: MUSTTAIL return normal_sldt_e16(cpu, op);
        case STRReg:  MUSTTAIL return normal_str_reg(cpu, op);
        case STRE16:  MUSTTAIL return normal_str_e16(cpu, op);
        case SGDT:    MUSTTAIL return normal_sgdt(cpu, op);
        case SIDT:    MUSTTAIL return normal_sidt(cpu, op);
        case Callback: MUSTTAIL return onExitSignal(cpu, op);
        case TestEnd:  MUSTTAIL return onTestEnd(cpu, op);
        default:       MUSTTAIL return op->pfn(cpu, op); // unimplemented/invalid opcodes
    }
}
#endif

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
#ifdef BOXEDWINE_MULTI_THREADED
// Guest threads run in pthreads and the layer's JS on the page's main thread (calls proxied
// there, the guest thread waits): hooksMutex guards the lists above, never held across a
// proxied call; hitMutex lets one hooked thread at a time into JS.
static std::mutex hooksMutex, hitMutex;
#define HOST_HOOKS_LOCK std::lock_guard<std::mutex> hostHooksLock(hooksMutex)
#else
#define HOST_HOOKS_LOCK
#endif

static bool findHostHook(U32 pid, U32 eip, U32* id) {
    HOST_HOOKS_LOCK;
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

#ifdef BOXEDWINE_MULTI_THREADED
static CPU* hookCpu; // the CPU that hit the hook, while JS runs (read on the main thread)
#else
static thread_local CPU* hookCpu; // the CPU that hit the hook, while JS runs
#endif

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
    HOST_HOOKS_LOCK;
    HostHook h = { nextHostHookId++, pid, eip };
    hostHooks.push_back(h);
    hostHookChanges.push_back(h);
    return h.id;
}

extern "C" EMSCRIPTEN_KEEPALIVE void bw_hook_remove(U32 id) {
    HOST_HOOKS_LOCK;
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
    HOST_HOOKS_LOCK;
    hostHookPrefetch.push_back({ addr, size });
}

#if defined(__EMSCRIPTEN__) && defined(BOXEDWINE_MULTI_THREADED)
// The page's window.boxedwineFrida lives on the main thread: same calls, proxied there.
static int boxedwine_frida_poll(U32 pid, U32 tid, U32 teb) {
    return MAIN_THREAD_EM_ASM_INT({
        var f = globalThis.boxedwineFrida;
        if (!f) return 0;
        if (!f.api) f.api = {read: _bw_read, scan: _bw_scan, reg: _bw_reg, setReg: _bw_set_reg, hookAdd: _bw_hook_add, hookRemove: _bw_hook_remove, prefetch: _bw_prefetch, malloc: _malloc, free: _free, heap: function() { return HEAPU8; }};
        return f.poll($0, $1, $2);
    }, pid, tid, teb);
}
static void boxedwine_frida_attach(U32 pid, U32 tid, U32 teb) {
    MAIN_THREAD_EM_ASM({ globalThis.boxedwineFrida.attach($0, $1, $2); }, pid, tid, teb);
}
static void boxedwine_frida_hit(U32 id, U32 pid, U32 tid) {
    MAIN_THREAD_EM_ASM({ globalThis.boxedwineFrida.hit($0, $1, $2); }, id, pid, tid);
}
#elif defined(__EMSCRIPTEN__)
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
    {
    HOST_HOOKS_LOCK;
    for (size_t i = 0; i < hostHookChanges.size();) {
        if (hostHookChanges[i].pid == pid) {
            thread->memory->removeCode(thread, hostHookChanges[i].eip, 1, false);
            hostHookChanges.erase(hostHookChanges.begin() + i);
        } else {
            i++;
        }
    }
    }
    static U32 count;
    if (++count % 1024) return;
    U32 teb = thread->cpu->seg[FS].address;
    for (int round = 0; round < 4; round++) {
        int r = boxedwine_frida_poll(pid, thread->id, teb);
        std::vector<std::pair<U32, U32>> prefetch;
        {
            HOST_HOOKS_LOCK;
            prefetch.swap(hostHookPrefetch);
        }
        for (auto& range : prefetch) {
            U32 first = range.first >> K_PAGE_SHIFT;
            U32 last = (U32)(std::min((U64)range.first + range.second, (U64)0x100000000ull) - 1) >> K_PAGE_SHIFT;
            for (U32 page = first; range.second && page <= last && page < K_NUMBER_OF_PAGES; page++) {
                MMU& mmu = getMemData(thread->memory)->mmu[page];
                if (mmu.getPageType() == PageType::File && thread->memory->canRead(page)) {
                    thread->memory->readb(page << K_PAGE_SHIFT); // loads the page
                }
            }
        }
        if (r == 1) {
            boxedwine_frida_attach(pid, thread->id, teb);
            hostHookSyscall(thread); // invalidate what the script hooked
        }
        if (r != 2) return;
    }
#endif
}

static void OPCALL hostHookOp(CPU* cpu, DecodedOp* op) {
    U32 id;
    if (findHostHook(cpu->thread->process->id, cpu->seg[CS].address + cpu->eip.u32, &id)) {
#ifdef __EMSCRIPTEN__
#ifdef BOXEDWINE_MULTI_THREADED
        std::lock_guard<std::mutex> hitLock(hitMutex);
#endif
        hookCpu = cpu;
        boxedwine_frida_hit(id, cpu->thread->process->id, cpu->thread->id);
        hookCpu = nullptr;
#endif
    }
    DecodedOp* real = reinterpret_cast<DecodedOp*>(op->data.nextJump);
#if defined(BOXEDWINE_DIRECT_NORMAL_DISPATCH) || defined(BOXEDWINE_WASM_JIT)
    MUSTTAIL return normalDispatch(cpu, real);
#else
    real->pfn(cpu, real);
#endif
}

static void initNormalOps() {
    if (normalOpsInitialized)
        return;
    normalOpsInitialized = 1;   
    for (int i=0;i<InstructionCount;i++) {
        normalOps[i] = normal_invalid;
    }
#define INIT_CPU(e, f) normalOps[e] = normal_##f;
#include "../common/cpu_init.h"
#include "../common/cpu_init_mmx.h"
#include "../common/cpu_init_sse.h"
#include "../common/cpu_init_sse2.h"
#include "../common/cpu_init_fpu.h"
#ifdef BOXEDWINE_MULTI_THREADED
#define INIT_CPU_LOCK(e, f) normalOps[e##_Lock] = normal_##f##_lock;
#include "../common/cpu_init_lock.h"
#undef INIT_CPU_LOCK
#endif
#undef INIT_CPU    
    
    normalOps[SLDTReg] = normal_sldt_reg;
    normalOps[SLDTE16] = normal_sldt_e16;
    normalOps[STRReg] = normal_str_reg;
    normalOps[STRE16] = normal_str_e16;
    normalOps[SIDT] = normal_sidt;
    normalOps[SGDT] = normal_sgdt;
    normalOps[Callback] = onExitSignal;
    normalOps[TestEnd] = onTestEnd;
}

#if defined(BOXEDWINE_JIT)
void OPCALL firstDynamicOp(CPU* cpu, DecodedOp* op);
#endif

NormalCPU::NormalCPU(KMemory* memory) : CPU(memory) {   
#if defined(BOXEDWINE_MULTI_THREADED) && defined(BOXEDWINE_WASM_JIT)
    wasmJitInitMemoryOrdering();
#endif
    initNormalOps();
#ifdef BOXEDWINE_JIT
    this->firstOp = firstDynamicOp;
    memcpy(this->calculateCF, memory->process->calculateCF, sizeof(calculateCF));
#else
    this->firstOp = nullptr;
#endif
}

OpCallback NormalCPU::getFunctionForOp(DecodedOp* op) {
    return normalOps[op->inst];
}

bool NormalCPU::isValidExecutableAddress(U32 address) {
    return (memory->getPageFlags(address >> K_PAGE_SHIFT) & PAGE_EXEC) != 0;
}

DecodedOp* NormalCPU::getOp(U32 startIp, U32 jumpTargetFlags) {
    if (!this->thread->process) // exit was called, don't need to pre-cache the next block
        return nullptr;

    DecodedOp* op = nullptr;
    while (true) {
        op = memory->getDecodedOp(startIp);
        if (op) {
            bool decodedBig = (op->flags2 & OP_FLAG2_DECODED_32BIT) != 0;
            if (decodedBig != this->isBig()) {
                memory->removeCode(this->thread, startIp, 1, false);
                continue;
            }
            break;
        }

        bool cacheFilledWhileLocking = false;
        {
            BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(memory->mutex);
            op = memory->getDecodedOp(startIp);
            if (op) {
                cacheFilledWhileLocking = true;
            } else {
                U32 opCount = 0;
                U32 eipLen = 0;
                U32 address = startIp;

                bool hooked = hostHookAt(this, startIp);
                op = decodeBlock(this, startIp, this->isBig(), opCount, eipLen, hooked ? 0 : 0xFFFFFFFF);
                if (!op) {
                    return nullptr;
                }
                if (hooked) {
                    // shortcut: the real op is not in the cache, so it leaks when this code is invalidated; fine for a few hooks.
                    op->pfn = getFunctionForOp(op);
                    DecodedOp* head = DecodedOp::alloc();
                    head->inst = Custom1;
                    head->pfn = hostHookOp;
                    head->len = op->len;
                    head->flags2 = op->flags2;
                    head->flags |= OP_FLAG_NO_JIT;
#ifdef BOXEDWINE_JIT
                    head->runCount = JIT_RUN_COUNT + 1;
#endif
                    head->data.nextJump = reinterpret_cast<DecodedOp**>(op);
                    op = head;
                }
                DecodedOp* nextOp = op;

                while (nextOp) {
                    if (!nextOp->pfn) {
                        nextOp->pfn = getFunctionForOp(nextOp);
                    }
                    if (memory->isAddressDynamic(address, nextOp->len)) {
                        nextOp->flags |= OP_FLAG_NO_JIT;
#ifdef BOXEDWINE_JIT
                        nextOp->runCount = JIT_RUN_COUNT + 1;
#endif
                    }
                    address += nextOp->len;
                    nextOp = nextOp->next;
                }
                this->thread->memory->addCode_nolock(startIp, eipLen, op, opCount);
            }
        }
        if (!cacheFilledWhileLocking) {
            break;
        }
    }
#ifdef BOXEDWINE_JIT
    op->flags2 |= jumpTargetFlags;
    if (jumpTargetFlags && (op->flags2 & OP_FLAG2_JUMP_TARGET_ASSUMED_FALSE)) {
        kpanic("JUMP_TARGET_ASSUMED_FALSE");
    }
#endif
    return op;
}

void NormalCPU::run() {
#if defined(BOXEDWINE_JIT) && !defined(BOXEDWINE_MULTI_THREADED) && !defined(BOXEDWINE_WASM_JIT)
    // Native JIT blocks can chain without returning through this dispatcher.
    // Periodically return so runThreadSlice can enforce its host-time limit.
    jitBranchBudget = 1024;
#endif
#ifdef BOXEDWINE_MULTI_THREADED
    U32 decodedOpEpoch = decodedOpCacheGlobalEpoch->load(std::memory_order_acquire);
    if (decodedOpCacheEpoch.load(std::memory_order_relaxed) != decodedOpEpoch) {
        // Drop the only DecodedOp pointer carried between dispatch blocks before
        // announcing that this CPU can no longer reference an older generation.
        nextOp = nullptr;
        memory->synchronizeDecodedOpCache(this, decodedOpEpoch);
    }
#endif
#if defined(BOXEDWINE_WASM_JIT) && !defined(__TEST)
    if (nextOp) {
        // WASM JIT fast exits can carry DecodedOp* values across code-cache
        // invalidation; refetch from the live cache before dereferencing.
        DecodedOp* liveOp = getMemData(memory)->opCache.get(getEipAddress());
        if (liveOp != nextOp) {
            nextOp = liveOp;
        }
    }
#endif
    if (!nextOp) {
        if (thread->terminating) {
            return;
        }
        nextOp = getNextOp();
        if (!nextOp) {
            thread->seg_instructionFetch(getEipAddress(), false);
            nextOp = getNextOp();
            if (!nextOp) {
                kpanic_fmt("Failed to get op for thread %d of process %d at address %x", thread->id, thread->process->id, getEipAddress());
            }
        }
    }
#ifdef BOXEDWINE_MULTI_THREADED
#ifdef BOXEDWINE_JIT
    // Most dispatches have no new signal. Avoid an atomic read-modify-write
    // in that case; a signal published after the load stays latched.
    if (this->jitSignalPending.load(std::memory_order_acquire)) {
        this->jitSignalPending.exchange(0, std::memory_order_acq_rel);
    }
#endif
    if (thread->pendingSignals && thread->runSignals()) {
        nextOp = getNextOp();
        return;
    }
#endif
    if (debugTrapActive) {
        runNextSingleOp();
        return;
    }
#ifdef BOXEDWINE_DIRECT_NORMAL_DISPATCH
    normalDispatch(this, nextOp);
#else
#ifdef BOXEDWINE_JIT
#ifndef BOXEDWINE_WASM_JIT
    if (!nextOp->pfnJitCode && nextOp->blockStart &&
        !(nextOp->flags2 & OP_FLAG2_JUMP_TARGET_ASSUMED_FALSE)) {
        startNewJIT(this, getEipAddress(), nextOp);
    }
#endif
    if (nextOp->runCount <= JIT_RUN_COUNT) {
        firstOp(this, nextOp);
    } else {
#if !defined(BOXEDWINE_MULTI_THREADED)
        this->blockInstructionCount += nextOp->blockOpCount;
#endif
        nextOp->pfn(this, nextOp);
    }
#else
    nextOp->pfn(this, nextOp);
#endif
#endif
}
