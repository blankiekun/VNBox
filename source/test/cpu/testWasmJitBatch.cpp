/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "boxedwine.h"
#include "testCPU.h"
#include "ksignal.h"
#if defined(BOXEDWINE_WASM_JIT)
#include "../../emulation/cpu/jit/jitCodeLifecycle.h"
#include "../../emulation/cpu/wasm/jitWasmCodeGen.h"
#include "../../emulation/cpu/wasm/wasmEmitter.h"
#include "../../emulation/cpu/wasm/wasmJitBatchPolicy.h"
#include "../../emulation/cpu/wasm/wasmModuleMerger.h"
#include "../../emulation/cpu/normal/normalCPU.h"
#include <climits>
#include <string>
#include <vector>
#include <emscripten.h>
void clearJitBlock(const std::vector<void*>& jitOps);
#ifdef BOXEDWINE_MULTI_THREADED
#include <emscripten/threading.h>
#include <memory>
#include <pthread.h>
#endif

void testWasmJitColdStringStub() {
    testNewInstruction(0);
    TestContext& context = testContext();
    CPU* cpu = context.cpu;
    cpu->big = 1;
    // Zero-based segments select the shared REP MOVS emitter.
    Seg savedDs = cpu->seg[DS];
    Seg savedEs = cpu->seg[ES];
    bool savedHasDs = context.process->hasSetSeg[DS];
    bool savedHasEs = context.process->hasSetSeg[ES];
    cpu->seg[DS].address = cpu->seg[ES].address = 0;
    context.process->hasSetSeg[DS] = context.process->hasSetSeg[ES] = false;

    U32 address = context.codeIp;
    testPushCode8(0xf3); testPushCode8(0xa4); // rep movsb
    testPushCode8(0xcd); testPushCode8(0x97); // TestEnd
    DecodedOp* op = cpu->getOp(address, 0);
    op->runCount = 0;
    startNewJIT(cpu, address, op);
    bool markedStub = (op->flags2 & OP_FLAG2_TRACED_STUB) != 0;

    cpu->seg[DS] = savedDs;
    cpu->seg[ES] = savedEs;
    context.process->hasSetSeg[DS] = savedHasDs;
    context.process->hasSetSeg[ES] = savedHasEs;
    if (!markedStub) {
        testFail("cold REP MOVS must mark its decoded instruction for traced fallback");
    }
}

#ifndef BOXEDWINE_MULTI_THREADED
void testWasmJitStBoundedDispatch() {
    extern S32 contextTimeRemaining;
    const S32 savedBudget = contextTimeRemaining;
    testNewInstruction(0);
    TestContext& context = testContext();
    CPU* cpu = context.cpu;
    constexpr U32 blockCount = 32;
    constexpr U32 blockBytes = 8;
    std::vector<DecodedOp*> blocks;
    for (U32 i = 0; i < blockCount; ++i) {
        testPushCode8(0x43); // inc ebx
        testPushCode8(0xb8); testPushCode32((i + 1) * blockBytes); // mov eax,next
        testPushCode8(0xff); testPushCode8(0xe0); // jmp eax: separate blocks
    }
    testPushCode8(0xcd); testPushCode8(0x97);
    for (U32 i = 0; i < blockCount; ++i) {
        U32 address = TEST_CODE_ADDRESS + i * blockBytes;
        DecodedOp* op = cpu->getOp(address, 0);
        startNewJIT(cpu, address, op);
        if (!op->pfnJitCode || op->pfn != context.process->startJITOp) {
            testFail("instruction budget test requires compiled block entries");
        }
        op->runCount = JIT_RUN_COUNT + 1;
        blocks.push_back(op);
    }
    cpu->eip.u32 = 0;
    cpu->reg[3].u32 = 0;
    cpu->nextOp = blocks[0];
    cpu->yield = false;
    cpu->blockInstructionCount = 0;
    contextTimeRemaining = 12;
    cpu->run();
    if (cpu->reg[3].u32 != 4 || cpu->blockInstructionCount != 12 || cpu->nextOp != blocks[4]) {
        testFail("ST JIT must return at the instruction budget with the next block pending");
    }
    // A partially used slice must count the first block as well as chained ones.
    cpu->blockInstructionCount = 9;
    cpu->run();
    if (cpu->reg[3].u32 != 5 || cpu->blockInstructionCount != 12 || cpu->nextOp != blocks[5]) {
        testFail("ST JIT must respect instructions already consumed in the slice");
    }
    for (U32 i = 0; i < blockCount && cpu->nextOp->inst != TestEnd; ++i) {
        cpu->blockInstructionCount = 0;
        cpu->run();
    }
    if (cpu->reg[3].u32 != blockCount || cpu->nextOp->inst != TestEnd) {
        testFail("budgeted dispatch must resume without skipping or repeating guest blocks");
    }
    contextTimeRemaining = savedBudget;
    cpu->nextOp = nullptr;
}

// Hot groups: chained standalone blocks that reach the entry threshold are compiled again into one module, keep
// running correctly, and a block whose slot is cleared while queued drops out of the queue.
void testWasmJitHotGroups() {
    testNewInstruction(0);
    TestContext& context = testContext();
    CPU* cpu = context.cpu;
    constexpr U32 blockCount = 8;
    constexpr U32 blockBytes = 8;
    constexpr U32 hotEntries = 4;
    wasmJitTestResetRuntimeBatching();
    wasmJitTestSetHotGroups(hotEntries, 1e12); // entries per period; the period never ends here: flush below
    for (U32 i = 0; i < blockCount; ++i) {
        testPushCode8(0x43); // inc ebx
        testPushCode8(0xb8); testPushCode32((i + 1) * blockBytes); // mov eax,next
        testPushCode8(0xff); testPushCode8(0xe0); // jmp eax: separate blocks
    }
    testPushCode8(0xcd); testPushCode8(0x97);
    std::vector<DecodedOp*> blocks;
    std::vector<void*> tier1;
    for (U32 i = 0; i < blockCount; ++i) {
        U32 address = TEST_CODE_ADDRESS + i * blockBytes;
        DecodedOp* op = cpu->getOp(address, 0);
        startNewJIT(cpu, address, op);
        if (!op->pfnJitCode || op->pfn != context.process->startJITOp) {
            testFail("hot group test requires compiled block entries");
            wasmJitTestResetRuntimeBatching();
            return;
        }
        op->runCount = JIT_RUN_COUNT + 1;
        blocks.push_back(op);
        tier1.push_back(op->pfnJitCode);
    }
    auto runOnce = [&]() {
        cpu->eip.u32 = 0;
        cpu->reg[3].u32 = 0;
        cpu->nextOp = blocks[0];
        cpu->yield = false;
        for (U32 i = 0; i < 4 * blockCount && cpu->nextOp->inst != TestEnd; ++i) {
            cpu->blockInstructionCount = 0;
            cpu->run();
        }
        return cpu->reg[3].u32 == blockCount && cpu->nextOp->inst == TestEnd;
    };
    for (U32 i = 0; i < hotEntries - 1; ++i) {
        if (!runOnce()) testFail("tier-1 blocks must run the chain");
    }
    if (wasmJitTestHotQueueSize() != 0) {
        testFail("blocks below the hot threshold must not queue");
    }
    if (!runOnce()) testFail("tier-1 blocks must run the chain at the threshold");
    if (wasmJitTestHotQueueSize() != blockCount || wasmJitTestHotModules() != 0) {
        testFail("every block at the hot threshold must queue once and wait for its group");
    }
    // A slot cleared while queued (code invalidated) leaves the queue; its op falls back to the interpreter.
    DecodedOp* dropped = blocks[3];
    clearJitBlock({dropped->pfnJitCode});
    dropped->pfnJitCode = nullptr;
    dropped->flags &= ~OP_FLAG_JIT;
    dropped->pfn = NormalCPU::getFunctionForOp(dropped);
    dropped->runCount = JIT_RUN_COUNT + 1;
    if (wasmJitTestHotQueueSize() != blockCount - 1) {
        testFail("a cleared slot must drop its hot queue entry");
    }
    wasmJitTestHotFlush(cpu); // what the end of a period does at a dispatcher exit
    if (!runOnce()) testFail("hot-group blocks must run the chain after the flush");
    if (wasmJitTestHotModules() != 1 || wasmJitTestHotBlocks() != blockCount - 1 || wasmJitTestHotQueueSize() != 0) {
        testFail("queued hot blocks must be compiled into one hot group");
    }
    for (U32 i = 0; i < blockCount; ++i) {
        if (i == 3) continue;
        if (!blocks[i]->pfnJitCode || blocks[i]->pfnJitCode == tier1[i] || blocks[i]->pfn != context.process->startJITOp ||
                (blocks[i]->flags2 & OP_FLAG2_WASM_JIT_PENDING)) {
            testFail("a hot block must be published at its new hot-group slot");
        }
    }
    // Hot-group code has no entry counter: running it many times never queues it again.
    for (U32 i = 0; i < 2 * hotEntries; ++i) {
        if (!runOnce()) testFail("hot-group blocks must run the chain");
    }
    if (wasmJitTestHotModules() != 1 || wasmJitTestHotQueueSize() != 0) {
        testFail("hot-group blocks must not queue again");
    }
    if (wasmJitTestRuntimeGroupCount() != 1) {
        testFail("the hot group must be one runtime group");
    }
    cpu->nextOp = nullptr;
    for (U32 i = 0; i < blockCount; ++i) {
        if (i != 3) context.memory->removeCodeBlock(TEST_CODE_ADDRESS + i * blockBytes, blocks[i], false);
    }
    if (wasmJitTestRuntimeGroupCount() != 0) {
        testFail("the hot group must be released with its last block");
    }
    // Catches: code invalidation (what an unmap or a write does) freeing a slot and keeping its queue entry. The freed
    // op can come back at another address with the same slot, and the flush would then compile it under the old eip.
    DecodedOp* requeued = cpu->getOp(TEST_CODE_ADDRESS, 0);
    startNewJIT(cpu, TEST_CODE_ADDRESS, requeued);
    requeued->runCount = JIT_RUN_COUNT + 1;
    wasmJitTestClearHotCounts();
    for (U32 i = 0; i < hotEntries; ++i) {
        cpu->eip.u32 = 0;
        cpu->nextOp = requeued;
        cpu->yield = false;
        cpu->blockInstructionCount = 0;
        cpu->run();
    }
    const U32 queued = wasmJitTestHotQueueSize();
    cpu->nextOp = nullptr;
    context.memory->removeCodeBlock(TEST_CODE_ADDRESS, requeued, false);
    if (!queued || wasmJitTestHotQueueSize() != queued - 1) {
        testFail("invalidated code must drop its hot queue entry (%u queued, %u after)", queued, wasmJitTestHotQueueSize());
    }
    wasmJitTestResetRuntimeBatching();
}

// Catches: a hot group that hits "out of memory" freeing the blocks' working code and leaving them sealed, which ran
// nothing and hung the main thread, on the first attempt or the retry; and any other op with startJITOp and no code
// doing the same in firstDynamicOp.
void testWasmJitHotGroupOom() {
    testNewInstruction(0);
    TestContext& context = testContext();
    CPU* cpu = context.cpu;
    constexpr U32 blockCount = 4;
    constexpr U32 blockBytes = 8;
    constexpr U32 hotEntries = 4;
    wasmJitTestResetRuntimeBatching();
    boxedwine_wasm_test_reset_oom_state();
    struct ResetOom {
        ~ResetOom() { boxedwine_wasm_test_reset_oom_state(); }
    } resetOom;
    wasmJitTestSetHotGroups(hotEntries, 1e12); // entries per period; the period never ends here: flush below
    for (U32 i = 0; i < blockCount; ++i) {
        testPushCode8(0x43); // inc ebx
        testPushCode8(0xb8); testPushCode32((i + 1) * blockBytes); // mov eax,next
        testPushCode8(0xff); testPushCode8(0xe0); // jmp eax: separate blocks
    }
    testPushCode8(0xcd); testPushCode8(0x97);
    std::vector<DecodedOp*> blocks;
    std::vector<void*> tier1;
    for (U32 i = 0; i < blockCount; ++i) {
        U32 address = TEST_CODE_ADDRESS + i * blockBytes;
        DecodedOp* op = cpu->getOp(address, 0);
        startNewJIT(cpu, address, op);
        if (!op->pfnJitCode || op->pfn != context.process->startJITOp) {
            testFail("hot group OOM test requires compiled block entries");
            wasmJitTestResetRuntimeBatching();
            return;
        }
        op->runCount = JIT_RUN_COUNT + 1;
        blocks.push_back(op);
        tier1.push_back(op->pfnJitCode);
    }
    auto runOnce = [&]() {
        cpu->eip.u32 = 0;
        cpu->reg[3].u32 = 0;
        cpu->nextOp = blocks[0];
        cpu->yield = false;
        for (U32 i = 0; i < 4 * blockCount && cpu->nextOp->inst != TestEnd; ++i) {
            cpu->blockInstructionCount = 0;
            cpu->run();
        }
        return cpu->reg[3].u32 == blockCount && cpu->nextOp->inst == TestEnd;
    };
    auto queueAll = [&]() {
        for (U32 i = 0; i < hotEntries; ++i) {
            if (!runOnce()) testFail("hot group OOM: blocks must run the chain");
        }
        wasmJitTestClearHotCounts();
        if (wasmJitTestHotQueueSize() != blockCount) {
            testFail("hot group OOM: every block must queue");
        }
    };
    queueAll();
    // Twice: the first attempt, and the retry that freeing the old slots used to make.
    boxedwine_wasm_test_force_next_module_oom();
    boxedwine_wasm_test_force_next_module_oom();
    wasmJitTestHotFlush(cpu);
    boxedwine_wasm_test_clear_forced_module_oom();
    if (!runOnce()) {
        testFail("hot group OOM: the guest must keep running");
    }
    for (U32 i = 0; i < blockCount; ++i) {
        DecodedOp* op = blocks[i];
        if (op->pfnJitCode != tier1[i] || !(op->flags & OP_FLAG_JIT) || op->pfn != context.process->startJITOp ||
                (op->flags2 & OP_FLAG2_WASM_JIT_PENDING) || op->runCount != JIT_RUN_COUNT + 1) {
            testFail("hot group OOM: a block must keep its old code");
        }
    }
    if (wasmJitTestPendingCount() != 0 || wasmJitTestSealedCount() != 0 || !wasmJitCompilationPaused()) {
        testFail("hot group OOM: no block stays pending, and compiling is paused");
    }
    // The pause ends by time, with no slot freed; the blocks then make their hot group.
    wasmJitTestOomTick(1000);
    wasmJitTestOomTick(1000 + 5000);
    if (wasmJitCompilationPaused()) {
        testFail("hot group OOM: the retry must end the pause");
    }
    queueAll();
    wasmJitTestHotFlush(cpu);
    if (!runOnce()) {
        testFail("hot group OOM: the group must run after the retry");
    }
    for (U32 i = 0; i < blockCount; ++i) {
        if (!blocks[i]->pfnJitCode || blocks[i]->pfnJitCode == tier1[i]) {
            testFail("hot group OOM: a block must be published at its new slot after the retry");
        }
    }
    // firstDynamicOp with startJITOp and no code: the op must run, not return with eip unmoved.
    DecodedOp* op = blocks[0];
    void* slot = op->pfnJitCode;
    decltype(op->runCount) runCount = op->runCount;
    op->pfnJitCode = nullptr;
    op->runCount = JIT_RUN_COUNT;
    cpu->eip.u32 = 0;
    cpu->reg[3].u32 = 0;
    cpu->nextOp = op;
    cpu->yield = false;
    cpu->blockInstructionCount = 0;
    cpu->run();
    op->pfnJitCode = slot;
    op->runCount = runCount;
    if (cpu->reg[3].u32 == 0) {
        testFail("hot group OOM: an op without code must run through the normal table");
    }
    cpu->nextOp = nullptr;
    for (U32 i = 0; i < blockCount; ++i) {
        context.memory->removeCodeBlock(TEST_CODE_ADDRESS + i * blockBytes, blocks[i], false);
    }
    wasmJitTestResetRuntimeBatching();
}

// Catches: a heap allocation failure in an instruction run through runNextSingleOp (cold ops in compiled blocks) being
// handled as a guest fault, so the guest retries it for ever and the main loop never gets to stop and report it.
extern bool ramPageTestFailNextAlloc;
void testWasmJitSingleOpOomPropagates() {
    testNewInstruction(0);
    TestContext& context = testContext();
    CPU* cpu = context.cpu;
    // A page no test has written: past the heap segment's mapped pages, reached through DS.
    const U32 offset = (32 + 1) << K_PAGE_SHIFT; // the test maps 32 pages per segment
    const U32 page = TEST_HEAP_ADDRESS + offset;
    if (context.memory->mmap(context.thread, page, K_PAGE_SIZE, K_PROT_READ | K_PROT_WRITE, K_MAP_FIXED | K_MAP_ANONYMOUS | K_MAP_PRIVATE, -1, 0) != page) {
        testFail("single op OOM: mapping a fresh page");
        return;
    }
    testPushCode8(0xa3); testPushCode32(offset); // mov [page],eax
    testPushCode8(0xcd); testPushCode8(0x97);
    cpu->eip.u32 = 0;
    cpu->nextOp = cpu->getNextOp();
    ramPageTestFailNextAlloc = true;
    bool threw = false;
    try {
        cpu->runNextSingleOp();
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    bool hookUsed = !ramPageTestFailNextAlloc;
    ramPageTestFailNextAlloc = false;
    if (!hookUsed) {
        testFail("single op OOM: the write must allocate the page");
    }
    if (!threw) {
        testFail("single op OOM: bad_alloc must leave runNextSingleOp");
    }
    context.memory->unmap(page, K_PAGE_SIZE);
    cpu->nextOp = nullptr;
}

// Leaf-call inlining: `call rel32` to a short callee is compiled into the caller's block. Catches: an inlined call
// that skips the guest push/pop of the return address (stack contents or esp differ), a forward jcc inside the callee
// taking the wrong path, a callee that changes its return address returning to the call site anyway, a write to the
// callee's code leaving the caller running the old copy, a fault inside the callee reported at the call (or without
// the return address on the stack), and jitInline off still inlining.
void testWasmJitInlineCall() {
    testNewInstruction(0);
    TestContext& context = testContext();
    CPU* cpu = context.cpu;
    cpu->big = 1;
    auto code = [&](U32 offset, std::initializer_list<U8> bytes) {
        context.codeIp = TEST_CODE_ADDRESS + offset;
        for (U8 b : bytes) testPushCode8(b);
    };
    auto call = [&](U32 offset, U32 target) {
        U32 rel = target - (offset + 5);
        code(offset, {0xe8, (U8)rel, (U8)(rel >> 8), (U8)(rel >> 16), (U8)(rel >> 24)});
    };
    // Caller: call A; inc ebx; call A; inc ebx; call B; inc ebx (skipped: B returns to 0x200); int 97.
    call(0x00, 0x100); code(0x05, {0x43});
    call(0x06, 0x100); code(0x0b, {0x43});
    call(0x0c, 0x140); code(0x11, {0x43, 0xcd, 0x97});
    // A: inc ecx; cmp ecx,100; ja +3; add edx,2; ret
    code(0x100, {0x41, 0x83, 0xf9, 0x64, 0x77, 0x03, 0x83, 0xc2, 0x02, 0xc3});
    // B: mov dword [esp],0x200; ret
    code(0x140, {0xc7, 0x04, 0x24, 0x00, 0x02, 0x00, 0x00, 0xc3});
    code(0x200, {0x46, 0xcd, 0x97}); // inc esi; int 97
    // Second caller: call C; int 97. C: mov eax,[disp32] (faults); ret. Signal handler: int 97.
    call(0x300, 0x180); code(0x305, {0xcd, 0x97});
    const U32 faultTarget = TEST_HEAP_ADDRESS + 0x4000;
    const U32 disp = faultTarget - cpu->seg[DS].address;
    code(0x180, {0xa1, (U8)disp, (U8)(disp >> 8), (U8)(disp >> 16), (U8)(disp >> 24), 0xc3});
    code(0x380, {0xcd, 0x97});

    auto compile = [&](U32 offset) {
        // Inlining only copies code that already ran (was decoded): both paths of A's jcc.
        cpu->getOp(TEST_CODE_ADDRESS + 0x100, 0);
        cpu->getOp(TEST_CODE_ADDRESS + 0x106, 0);
        cpu->getOp(TEST_CODE_ADDRESS + 0x140, 0);
        cpu->getOp(TEST_CODE_ADDRESS + 0x180, 0);
        DecodedOp* op = cpu->getOp(TEST_CODE_ADDRESS + offset, 0);
        startNewJIT(cpu, TEST_CODE_ADDRESS + offset, op);
        op->runCount = JIT_RUN_COUNT + 1;
        return op;
    };
    auto run = [&](DecodedOp* op, U32 offset) {
        cpu->eip.u32 = offset;
        cpu->nextOp = op;
        cpu->yield = false;
        for (U32 i = 0; i < 64 && (!cpu->nextOp || cpu->nextOp->inst != TestEnd); ++i) {
            cpu->blockInstructionCount = 0;
            try {
                cpu->run();
            } catch (int) {
                cpu->nextOp = nullptr; // a guest fault delivered its signal (as in testRunCPU)
            }
        }
        return cpu->nextOp && cpu->nextOp->inst == TestEnd;
    };
    const U32 stackTop = cpu->reg[4].u32;
    auto runCaller = [&](DecodedOp* op) {
        cpu->reg[1].u32 = 99; cpu->reg[2].u32 = 0; cpu->reg[3].u32 = 0; cpu->reg[6].u32 = 0;
        cpu->reg[4].u32 = stackTop;
        context.memory->writed(cpu->seg[SS].address + stackTop - 4, 0);
        if (!run(op, 0)) testFail("inlined calls: caller did not reach its end");
        if (cpu->reg[1].u32 != 101 || cpu->reg[3].u32 != 2 || cpu->reg[6].u32 != 1 || cpu->reg[4].u32 != stackTop) {
            testFail("inlined calls: ecx=%u ebx=%u esi=%u esp=%x", cpu->reg[1].u32, cpu->reg[3].u32, cpu->reg[6].u32, cpu->reg[4].u32);
        }
        if (context.memory->readd(cpu->seg[SS].address + stackTop - 4) != 0x200) {
            testFail("inlined calls must push the return address on the guest stack");
        }
        return cpu->reg[2].u32;
    };

    U64 before = wasmJitTestInlinedCalls();
    DecodedOp* caller = compile(0);
    if (wasmJitTestInlinedCalls() - before != 3 || !caller->pfnJitCode) {
        testFail("three direct calls to leaf callees must be inlined (%llu)", (unsigned long long)(wasmJitTestInlinedCalls() - before));
    }
    if (runCaller(caller) != 2) testFail("inlined callee jcc: edx=%u, expected 2", cpu->reg[2].u32);
    DecodedOp* calleeA = context.memory->getDecodedOp(TEST_CODE_ADDRESS + 0x100);
    if (calleeA && calleeA->pfnJitCode) testFail("an inlined callee must not run as its own block");

    // The callee's code changes (add edx,2 -> add edx,5): the caller must be invalidated and compile again.
    context.memory->writeb(TEST_CODE_ADDRESS + 0x108, 5);
    if (caller->pfnJitCode && context.memory->getDecodedOp(TEST_CODE_ADDRESS) == caller) {
        testFail("a write to an inlined callee must invalidate its caller");
    }
    caller = compile(0);
    if (runCaller(caller) != 5) testFail("after the callee changed: edx=%u, expected 5", cpu->reg[2].u32);

    // jitInline=0
    setenv("BOXEDWINE_WASM_JIT_INLINE", "0", 1);
    context.memory->removeCodeBlock(TEST_CODE_ADDRESS, caller, false);
    before = wasmJitTestInlinedCalls();
    caller = compile(0);
    unsetenv("BOXEDWINE_WASM_JIT_INLINE");
    if (wasmJitTestInlinedCalls() != before) testFail("jitInline off must not inline");
    if (runCaller(caller) != 5) testFail("not inlined: edx=%u, expected 5", cpu->reg[2].u32);
    // A fault inside the inlined callee reports the callee's eip, with the return address pushed.
    Seg savedSegs[7];
    memcpy(savedSegs, cpu->seg, sizeof(savedSegs));
    KSigAction savedAction = context.process->sigActions[K_SIGSEGV];
    KSigAction& action = context.process->sigActions[K_SIGSEGV];
    action.reset(); action.flags = K_SA_SIGINFO; action.handlerAndSigAction = TEST_CODE_ADDRESS + 0x380;
    const U32 page = faultTarget & ~K_PAGE_MASK;
    context.memory->mprotect(context.thread, page, K_PAGE_SIZE, 0);
    before = wasmJitTestInlinedCalls();
    DecodedOp* faulting = compile(0x300);
    if (wasmJitTestInlinedCalls() - before != 1) testFail("a callee that can fault must still be inlined");
    cpu->reg[4].u32 = stackTop;
    const U32 stackBase = cpu->seg[SS].address;
    run(faulting, 0x300);
    const U32 uc = action.sigInfo[0] == K_SIGSEGV ? cpu->reg[2].u32 : 0;
    if (!uc) {
        testFail("a fault inside an inlined callee must raise SIGSEGV");
    } else {
        const U32 faultEip = context.memory->readd(uc + 0x4c);
        const U32 faultEsp = context.memory->readd(uc + 0x30);
        if (faultEip != 0x180 || faultEsp != stackTop - 4 || context.memory->readd(stackBase + faultEsp) != 0x305) {
            testFail("inlined callee fault: eip=%x esp=%x", faultEip, faultEsp);
        }
    }
    context.memory->mprotect(context.thread, page, K_PAGE_SIZE, K_PROT_READ | K_PROT_WRITE);
    context.process->sigActions[K_SIGSEGV] = savedAction;
    memcpy(cpu->seg, savedSegs, sizeof(savedSegs));

    // Catches: inline dependencies piling up on a callee page nothing ever writes to, when the caller (a plugin DLL)
    // is unloaded and loaded again, at the same address or another one. Callers on their own page: removing one
    // never scans the callee's page.
    constexpr U32 deadCallerA = K_PAGE_SIZE;
    constexpr U32 deadCallerB = K_PAGE_SIZE + 0x40;
    code(0x400, {0x83, 0xc2, 0x01, 0xc3}); // add edx,1; ret
    call(deadCallerA, 0x400); code(deadCallerA + 5, {0xcd, 0x97});
    call(deadCallerB, 0x400); code(deadCallerB + 5, {0xcd, 0x97});
    auto compileDeadCaller = [&](U32 offset) {
        cpu->getOp(TEST_CODE_ADDRESS + 0x400, 0);
        return compile(offset);
    };
    before = wasmJitTestInlinedCalls();
    compileDeadCaller(deadCallerA);
    if (wasmJitTestInlinedCalls() - before != 1) testFail("a caller on another page must inline its callee");
    const U32 depsOfOneCaller = wasmJitTestInlineDepCount();
    for (U32 i = 0; i < 4; ++i) {
        context.memory->removeCode(context.thread, TEST_CODE_ADDRESS + deadCallerA, 0x10, false);
        compileDeadCaller(deadCallerA);
        if (wasmJitTestInlineDepCount() != depsOfOneCaller) {
            testFail("a caller loaded again at its address must not add inline deps (%u, expected %u)", wasmJitTestInlineDepCount(), depsOfOneCaller);
        }
    }
    context.memory->removeCode(context.thread, TEST_CODE_ADDRESS + deadCallerA, 0x10, false);
    DecodedOp* liveCaller = compileDeadCaller(deadCallerB);
    if (wasmJitTestInlineDepCount() != depsOfOneCaller) {
        testFail("a caller loaded at another address must drop the old inline deps (%u, expected %u)", wasmJitTestInlineDepCount(), depsOfOneCaller);
    }
    // A live caller's dep survives that pruning: a write to the callee still reaches it.
    compileDeadCaller(deadCallerA);
    context.memory->writeb(TEST_CODE_ADDRESS + 0x402, 2);
    if (context.memory->getDecodedOp(TEST_CODE_ADDRESS + deadCallerB) == liveCaller && liveCaller->pfnJitCode) {
        testFail("a write to an inlined callee must still invalidate a caller whose page's deps were pruned");
    }

    cpu->nextOp = nullptr;
}

// Guest code laid out at fixed offsets from TEST_CODE_ADDRESS, compiled and run a block at a time.
namespace {
struct JitShape {
    TestContext& context = testContext();
    CPU* cpu = context.cpu;
    U32 stackTop = 0;
    JitShape() {
        testNewInstruction(0);
        cpu = context.cpu;
        cpu->big = 1;
        stackTop = cpu->reg[4].u32;
    }
    void code(U32 offset, std::initializer_list<U8> bytes) {
        context.codeIp = TEST_CODE_ADDRESS + offset;
        for (U8 b : bytes) testPushCode8(b);
    }
    void rel32(U32 offset, std::initializer_list<U8> opcode, U32 target) { // call, jmp and long jcc forms
        code(offset, opcode);
        U32 rel = target - (offset + (U32)opcode.size() + 4);
        testPushCode32(rel);
    }
    DecodedOp* compile(U32 offset) {
        DecodedOp* op = cpu->getOp(TEST_CODE_ADDRESS + offset, 0);
        startNewJIT(cpu, TEST_CODE_ADDRESS + offset, op);
        if (!op->pfnJitCode || op->pfn != context.process->startJITOp) {
            testFail("test block at %x did not compile", offset);
        }
        op->runCount = JIT_RUN_COUNT + 1;
        return op;
    }
    // Compile, and catch a register the block loads from the CPU struct a second time: only a boundary that forgot it
    // makes it do that.
    DecodedOp* compileNoReload(U32 offset, const char* what) {
        wasmJitTestResetRegLoads();
        DecodedOp* op = compile(offset);
        if (wasmJitTestMaxRegLoads() > 1) {
            testFail("%s: the path that stays reloads a register (%u loads)", what, wasmJitTestMaxRegLoads());
        }
        return op;
    }
    void run(DecodedOp* op, U32 offset) {
        cpu->eip.u32 = offset;
        cpu->nextOp = op;
        cpu->yield = false;
        for (U32 i = 0; i < 64 && (!cpu->nextOp || cpu->nextOp->inst != TestEnd); ++i) {
            cpu->blockInstructionCount = 0;
            cpu->run();
        }
        if (!cpu->nextOp || cpu->nextOp->inst != TestEnd) testFail("test block at %x did not reach its end", offset);
    }
    void regs(U32 eax, U32 ecx, U32 edx, U32 ebx, U32 ebp, U32 esi, U32 edi) {
        cpu->reg[0].u32 = eax; cpu->reg[1].u32 = ecx; cpu->reg[2].u32 = edx; cpu->reg[3].u32 = ebx;
        cpu->reg[4].u32 = stackTop; cpu->reg[5].u32 = ebp; cpu->reg[6].u32 = esi; cpu->reg[7].u32 = edi;
    }
    void xmmFloats(U32 index, float value) {
        float lanes[4] = {value, value, value, value};
        memcpy(&cpu->xmm[index], lanes, sizeof(lanes));
    }
    float xmmLane(U32 index, U32 lane) {
        float lanes[4];
        memcpy(lanes, &cpu->xmm[index], sizeof(lanes));
        return lanes[lane];
    }
};
}

// Catches: the cold arm of a jcc side exit, a CMOV select or the SSE divide guard changing what the path that stays
// sees: a dirty register lost or read stale, an arm's state leaking into the fall-through, the divide run twice.
void testWasmJitColdArmsKeepRegisters() {
    extern S32 contextTimeRemaining;
    const S32 savedBudget = contextTimeRemaining;
    contextTimeRemaining = 0x7fffffff;
    JitShape t;
    CPU* cpu = t.cpu;
    enum { rEax, rEcx, rEdx, rEbx, rEsp, rEbp, rEsi, rEdi };

    // jz side exit with dirty registers. Taken: the target block reads eax from the CPU struct.
    t.code(0x000, {0x89, 0xd8, 0x89, 0xf1, 0x85, 0xff}); // mov eax,ebx; mov ecx,esi; test edi,edi
    t.rel32(0x006, {0x0f, 0x84}, 0x200); // jz 0x200
    t.code(0x00c, {0x01, 0xc8, 0x01, 0xf0, 0x89, 0xc2, 0xcd, 0x97}); // add eax,ecx; add eax,esi; mov edx,eax; int 97
    t.code(0x200, {0x89, 0xc5, 0x89, 0xcf, 0xcd, 0x97}); // mov ebp,eax; mov edi,ecx; int 97
    DecodedOp* op = t.compileNoReload(0x000, "jz side exit");
    t.regs(0, 0, 0, 10, 0, 3, 0);
    t.run(op, 0);
    if (cpu->reg[rEbp].u32 != 10 || cpu->reg[rEdi].u32 != 3 || cpu->reg[rEdx].u32 != 0) {
        testFail("side exit taken: ebp=%u edi=%u edx=%u", cpu->reg[rEbp].u32, cpu->reg[rEdi].u32, cpu->reg[rEdx].u32);
    }
    t.regs(0, 0, 0, 10, 0, 3, 1);
    t.run(op, 0);
    if (cpu->reg[rEdx].u32 != 16 || cpu->reg[rEbp].u32 != 0) {
        testFail("side exit not taken: edx=%u ebp=%u", cpu->reg[rEdx].u32, cpu->reg[rEbp].u32);
    }

    // The same with flags from an earlier block: the condition goes through the lazy flag helper.
    t.code(0x300, {0x85, 0xff}); // test edi,edi
    t.rel32(0x302, {0xe9}, 0x310); // jmp 0x310
    t.code(0x310, {0x89, 0xd8}); // mov eax,ebx
    t.rel32(0x312, {0x0f, 0x84}, 0x200); // jz 0x200
    t.code(0x318, {0x01, 0xd8, 0x89, 0xc2, 0xcd, 0x97}); // add eax,ebx; mov edx,eax; int 97
    t.compileNoReload(0x310, "side exit on earlier flags");
    op = t.compile(0x300);
    t.regs(0, 0, 0, 10, 0, 0, 0);
    t.run(op, 0x300);
    if (cpu->reg[rEbp].u32 != 10) testFail("side exit on earlier flags, taken: ebp=%u", cpu->reg[rEbp].u32);
    t.regs(0, 0, 0, 10, 0, 0, 1);
    t.run(op, 0x300);
    if (cpu->reg[rEdx].u32 != 20) testFail("side exit on earlier flags, not taken: edx=%u", cpu->reg[rEdx].u32);

    // x87 values held in locals across the side exit: the arm writes them back, the path that stays keeps them.
    const U32 m1 = TEST_HEAP_ADDRESS + 0x100, m2 = TEST_HEAP_ADDRESS + 0x104;
    const U32 d1 = m1 - cpu->seg[DS].address, d2 = m2 - cpu->seg[DS].address;
    t.code(0x400, {0xd9, 0xe8, 0x85, 0xff}); // fld1; test edi,edi
    t.rel32(0x404, {0x0f, 0x84}, 0x480); // jz 0x480
    t.code(0x40a, {0xd9, 0xe8, 0xde, 0xc1, 0xdb, 0x1d, (U8)d1, (U8)(d1 >> 8), (U8)(d1 >> 16), (U8)(d1 >> 24), 0xcd, 0x97}); // fld1; faddp; fistp [m1]; int 97
    t.code(0x480, {0xdb, 0x1d, (U8)d2, (U8)(d2 >> 8), (U8)(d2 >> 16), (U8)(d2 >> 24), 0xcd, 0x97}); // fistp [m2]; int 97
    op = t.compile(0x400);
    t.context.memory->writed(m1, 0); t.context.memory->writed(m2, 0);
    t.regs(0, 0, 0, 0, 0, 0, 0);
    t.run(op, 0x400);
    if (t.context.memory->readd(m2) != 1 || t.context.memory->readd(m1) != 0) testFail("x87 side exit taken: [m2]=%u", t.context.memory->readd(m2));
    t.regs(0, 0, 0, 0, 0, 0, 1);
    t.run(op, 0x400);
    if (t.context.memory->readd(m1) != 2) testFail("x87 side exit not taken: [m1]=%u", t.context.memory->readd(m1));

    // cmovz eax,esi fused with its cmp. Both outcomes, and registers stay mapped after it.
    t.code(0x500, {0x89, 0xd8, 0x39, 0xd1, 0x0f, 0x44, 0xc6, 0x01, 0xf0, 0x01, 0xd1, 0x89, 0xcf, 0xcd, 0x97}); // mov eax,ebx; cmp ecx,edx; cmovz eax,esi; add eax,esi; add ecx,edx; mov edi,ecx; int 97
    op = t.compileNoReload(0x500, "cmov");
    t.regs(0, 7, 7, 1, 0, 100, 0);
    t.run(op, 0x500);
    if (cpu->reg[rEax].u32 != 200 || cpu->reg[rEdi].u32 != 14) testFail("cmov taken: eax=%u edi=%u", cpu->reg[rEax].u32, cpu->reg[rEdi].u32);
    t.regs(0, 7, 8, 1, 0, 100, 0);
    t.run(op, 0x500);
    if (cpu->reg[rEax].u32 != 101 || cpu->reg[rEdi].u32 != 15) testFail("cmov not taken: eax=%u edi=%u", cpu->reg[rEax].u32, cpu->reg[rEdi].u32);

    // cmovz ax,si keeps eax's upper half on both outcomes.
    t.code(0x540, {0x39, 0xd1, 0x66, 0x0f, 0x44, 0xc6, 0xcd, 0x97}); // cmp ecx,edx; cmovz ax,si; int 97
    op = t.compile(0x540);
    t.regs(0x11112222, 7, 7, 0, 0, 0x33334444, 0);
    t.run(op, 0x540);
    if (cpu->reg[rEax].u32 != 0x11114444) testFail("cmov16 taken: eax=%x", cpu->reg[rEax].u32);
    t.regs(0x11112222, 7, 8, 0, 0, 0x33334444, 0);
    t.run(op, 0x540);
    if (cpu->reg[rEax].u32 != 0x11112222) testFail("cmov16 not taken: eax=%x", cpu->reg[rEax].u32);

    // cmov alone in a block: not fused, flags from the block before, dst dirty first.
    t.code(0x580, {0x85, 0xff}); // test edi,edi
    t.rel32(0x582, {0xe9}, 0x590); // jmp 0x590
    t.code(0x590, {0x89, 0xd8, 0x0f, 0x44, 0xc6, 0x89, 0xc2, 0xcd, 0x97}); // mov eax,ebx; cmovz eax,esi; mov edx,eax; int 97
    t.compile(0x590);
    op = t.compile(0x580);
    t.regs(0, 0, 0, 5, 0, 9, 0);
    t.run(op, 0x580);
    if (cpu->reg[rEdx].u32 != 9) testFail("cmov on earlier flags, taken: edx=%u", cpu->reg[rEdx].u32);
    t.regs(0, 0, 0, 5, 0, 9, 1);
    t.run(op, 0x580);
    if (cpu->reg[rEdx].u32 != 5) testFail("cmov on earlier flags, not taken: edx=%u", cpu->reg[rEdx].u32);

    // SSE divide guard, packed and in a scalar region. Unmasked, the op runs in the arm and the block leaves: once only.
    t.code(0x600, {0x0f, 0x58, 0xc1, 0x0f, 0x5e, 0xc1, 0x0f, 0x58, 0xc1, 0x0f, 0x28, 0xd0, 0xcd, 0x97}); // addps xmm0,xmm1; divps xmm0,xmm1; addps xmm0,xmm1; movaps xmm2,xmm0; int 97
    t.code(0x640, {0xf3, 0x0f, 0x58, 0xc1, 0xf3, 0x0f, 0x5e, 0xc1, 0xf3, 0x0f, 0x58, 0xc1, 0xcd, 0x97}); // addss xmm0,xmm1; divss xmm0,xmm1; addss xmm0,xmm1; int 97
    DecodedOp* packed = t.compileNoReload(0x600, "divps guard");
    DecodedOp* scalar = t.compileNoReload(0x640, "divss guard");
    for (U32 unmasked = 0; unmasked < 2; ++unmasked) {
        const U32 saved = cpu->sseDivExceptionsUnmasked;
        cpu->sseDivExceptionsUnmasked = unmasked;
        t.xmmFloats(0, 1.0f); t.xmmFloats(1, 2.0f); t.xmmFloats(2, 0.0f);
        t.run(packed, 0x600);
        if (t.xmmLane(0, 3) != 3.5f || t.xmmLane(2, 0) != 3.5f) testFail("divps unmasked=%u: xmm0=%f xmm2=%f", unmasked, t.xmmLane(0, 3), t.xmmLane(2, 0));
        t.xmmFloats(0, 1.0f); t.xmmFloats(1, 2.0f);
        t.run(scalar, 0x640);
        if (t.xmmLane(0, 0) != 3.5f || t.xmmLane(0, 1) != 1.0f) testFail("divss unmasked=%u: xmm0=%f,%f", unmasked, t.xmmLane(0, 0), t.xmmLane(0, 1));
        cpu->sseDivExceptionsUnmasked = saved;
    }
    contextTimeRemaining = savedBudget;
    cpu->nextOp = nullptr;
}

// Catches: a stale type in the CPU struct, and a setcc or cmov some ops after its producer reading the wrong value (a
// register overwritten since, a high-byte result).
void testWasmJitLazyFlagStores() {
    extern S32 contextTimeRemaining;
    const S32 savedBudget = contextTimeRemaining;
    contextTimeRemaining = 0x7fffffff;
    JitShape t;
    CPU* cpu = t.cpu;
    enum { rEax, rEcx, rEdx, rEbx, rEsp, rEbp, rEsi, rEdi };
    auto zf = [&]() { cpu->fillFlags(); return (cpu->flags & ZF) != 0; };

    // Two adds with live flags: each stores its type.
    t.code(0x00, {0x01, 0xd8, 0x0f, 0x94, 0xc2, 0x01, 0xd9, 0x0f, 0x94, 0xc6, 0xcd, 0x97}); // add eax,ebx; setz dl; add ecx,ebx; setz dh; int 97
    DecodedOp* op = t.compile(0x00);
    t.regs(1, 5, 0, (U32)-1, 0, 0, 0);
    t.run(op, 0x00);
    if ((cpu->reg[rEdx].u32 & 0xffff) != 0x0001 || zf()) testFail("add setz add setz (1): edx=%x", cpu->reg[rEdx].u32);
    t.regs(1, (U32)-1, 0, 1, 0, 0, 0);
    t.run(op, 0x00);
    if ((cpu->reg[rEdx].u32 & 0xffff) != 0x0100 || !zf()) testFail("add setz add setz (2): edx=%x", cpu->reg[rEdx].u32);

    // Different types: each store stays, and the last op's type is what the CPU struct has.
    t.code(0x40, {0x01, 0xd8, 0x0f, 0x94, 0xc2, 0x29, 0xd9, 0x0f, 0x94, 0xc6, 0x01, 0xdf, 0xcd, 0x97}); // add eax,ebx; setz dl; sub ecx,ebx; setz dh; add edi,ebx; int 97
    op = t.compile(0x40);
    t.regs(1, 7, 0, 7, 0, 0, 1); // the sub is zero, the last add isn't: a stale SUB type over its dst/src would say zero differently
    t.run(op, 0x40);
    if ((cpu->reg[rEdx].u32 & 0xffff) != 0x0100 || cpu->reg[rEdi].u32 != 8 || zf() || cpu->lazyFlagType == FLAGS_SUB32) {
        testFail("add sub add: edx=%x edi=%u type=%u", cpu->reg[rEdx].u32, cpu->reg[rEdi].u32, (U32)cpu->lazyFlagType);
    }

    // A result used across other ops: the producer's register is overwritten before the setcc and the cmov read it.
    t.code(0x80, {0x01, 0xd8, 0x89, 0xc8, 0x89, 0xd5, 0x0f, 0x94, 0xc2, 0x0f, 0x44, 0xf7, 0xcd, 0x97}); // add eax,ebx; mov eax,ecx; mov ebp,edx; setz dl; cmovz esi,edi; int 97
    op = t.compile(0x80);
    t.regs(1, 50, 0, (U32)-1, 0, 2, 3);
    t.run(op, 0x80);
    if ((cpu->reg[rEdx].u32 & 0xff) != 1 || cpu->reg[rEsi].u32 != 3 || cpu->reg[rEax].u32 != 50) {
        testFail("forwarded zero result: edx=%x esi=%u eax=%u", cpu->reg[rEdx].u32, cpu->reg[rEsi].u32, cpu->reg[rEax].u32);
    }
    t.regs(1, 50, 0, 1, 0, 2, 3);
    t.run(op, 0x80);
    if ((cpu->reg[rEdx].u32 & 0xff) != 0 || cpu->reg[rEsi].u32 != 2) {
        testFail("forwarded non-zero result: edx=%x esi=%u", cpu->reg[rEdx].u32, cpu->reg[rEsi].u32);
    }

    // The same from a high-byte register, with other bits set above it.
    t.code(0xc0, {0x00, 0xdc, 0x89, 0xd5, 0x0f, 0x94, 0xc2, 0xcd, 0x97}); // add ah,bl; mov ebp,edx; setz dl; int 97
    op = t.compile(0xc0);
    t.regs(0x12340100, 0, 0, 0xff, 0, 0, 0);
    t.run(op, 0xc0);
    if ((cpu->reg[rEdx].u32 & 0xff) != 1 || cpu->reg[rEax].u32 != 0x12340000) {
        testFail("forwarded high-byte zero: edx=%x eax=%x", cpu->reg[rEdx].u32, cpu->reg[rEax].u32);
    }
    t.regs(0x12340100, 0, 0, 0x01, 0, 0, 0);
    t.run(op, 0xc0);
    if ((cpu->reg[rEdx].u32 & 0xff) != 0) testFail("forwarded high-byte non-zero: edx=%x", cpu->reg[rEdx].u32);
    contextTimeRemaining = savedBudget;
    cpu->nextOp = nullptr;
}

// Catches: a result forwarded to the next op or a side exit giving other flags or registers than the CPU struct path
// would. Random runs of register ops in one block against the same ops one block each: there nothing is forwarded and
// each setcc, cmov and jcc reads the type and values in the CPU struct. Both sides are JIT-compiled, so a wrong cmov
// select changes both alike: 105 and 106 catch that.
void testWasmJitFlagsMatchOneBlockPerOp() {
    extern S32 contextTimeRemaining;
    const S32 savedBudget = contextTimeRemaining;
    contextTimeRemaining = 0x7fffffff;
    struct Unit { std::vector<U8> b; S32 relAt = -1; }; // relAt: a jcc's rel32 to the stub
    U32 seed = 0x9e3779b9;
    auto rnd = [&]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
    auto value = [&]() {
        static const U32 edges[] = {0, 1, 2, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000, 0x7fffffff, 0x80000000, 0xffffffff};
        return (rnd() & 3) ? edges[rnd() % 14] : rnd();
    };
    auto r32 = [&]() { static const U8 gp[] = {0, 1, 2, 3, 5, 6}; return gp[rnd() % 6]; }; // edi marks the stub, esp stays
    auto pushImm = [](std::vector<U8>& b, U32 imm, U32 bytes) { for (U32 i = 0; i < bytes; ++i) b.push_back((U8)(imm >> (8 * i))); };
    auto cc = [&]() { static const U8 common[] = {4, 5, 8, 9, 10, 11}; return (U8)((rnd() & 3) ? common[rnd() % 6] : rnd() % 16); }; // z, s, p are the forwarded ones
    auto makeOp = [&]() {
        std::vector<U8> b;
        const U32 kind = rnd() % 100, width = rnd() % 3; // width: 0 32 bits, 1 16 bits, 2 8 bits
        const U8 d = width == 2 ? (U8)(rnd() % 8) : r32(), s = width == 2 ? (U8)(rnd() % 8) : r32();
        const U8 rr = (U8)(0xc0 | (s << 3) | d);
        const U32 immBytes = width == 0 ? 4 : width == 1 ? 2 : 1;
        if (width == 1 && kind < 65) b.push_back(0x66);
        if (kind < 30) { // add sub cmp test and or xor adc sbb, register form
            static const U8 alu[] = {0x01, 0x29, 0x39, 0x85, 0x21, 0x09, 0x31, 0x11, 0x19};
            b.push_back((U8)(alu[rnd() % (kind < 27 ? 7 : 9)] - (width == 2 ? 1 : 0)));
            b.push_back(rr);
        } else if (kind < 45) { // the same with an immediate
            static const U8 ext[] = {0, 1, 4, 5, 6, 7};
            b.push_back(width == 2 ? 0x80 : 0x81);
            b.push_back((U8)(0xc0 | (ext[rnd() % 6] << 3) | d));
            pushImm(b, value(), immBytes);
        } else if (kind < 55) { // mov
            b.push_back(width == 2 ? 0x88 : 0x89);
            b.push_back(rr);
        } else if (kind < 60) {
            b.push_back((U8)((width == 2 ? 0xb0 : 0xb8) + d));
            pushImm(b, value(), immBytes);
        } else if (kind < 65) { // lea d,[base+index+disp8]
            if (width == 2) b.push_back(0x66);
            b.push_back(0x8d); b.push_back((U8)(0x44 | (r32() << 3))); b.push_back((U8)((r32() << 3) | r32())); b.push_back((U8)rnd());
        } else if (kind < 78) { // setcc r8
            b.push_back(0x0f); b.push_back((U8)(0x90 + cc())); b.push_back((U8)(0xc0 | (rnd() % 8)));
        } else if (kind < 88) { // cmovcc r32
            b.push_back(0x0f); b.push_back((U8)(0x40 + cc())); b.push_back((U8)(0xc0 | (r32() << 3) | r32()));
        } else { // ops that drop the type or keep CF: inc dec neg shl shr sar
            const U8 r = r32();
            switch (rnd() % 6) {
            case 0: b.push_back((U8)(0x40 + r)); break;
            case 1: b.push_back((U8)(0x48 + r)); break;
            case 2: b.push_back(0xf7); b.push_back((U8)(0xd8 + r)); break;
            case 3: b.push_back(0xc1); b.push_back((U8)(0xe0 + r)); b.push_back((U8)(rnd() % 32)); break;
            case 4: b.push_back(0xc1); b.push_back((U8)(0xe8 + r)); b.push_back((U8)(rnd() % 32)); break;
            default: b.push_back(0xc1); b.push_back((U8)(0xf8 + r)); b.push_back((U8)(rnd() % 32)); break;
            }
        }
        return b;
    };
    const U32 trials = 1500, flagMask = CF | PF | AF | ZF | SF | OF, stub = 0x800, perOp = 0x200, stride = 0x20;
    for (U32 trial = 0; trial < trials; ++trial) {
        JitShape t; // new memory and decoded ops
        CPU* cpu = t.cpu;
        std::vector<Unit> units(2 + rnd() % 14);
        for (auto& u : units) {
            const U32 what = rnd() % 100;
            if (what < 6) { // a side exit
                u.b = {0x0f, (U8)(0x80 + cc()), 0, 0, 0, 0};
                u.relAt = 2;
            } else {
                u.b = makeOp();
                if (what < 12) { // a jcc over the op: a label in the block
                    u.b.insert(u.b.begin(), {(U8)(0x70 + cc()), (U8)u.b.size()});
                }
            }
        }
        auto place = [&](U32 offset, const Unit& u) {
            t.context.codeIp = TEST_CODE_ADDRESS + offset;
            for (size_t i = 0; i < u.b.size(); ++i) {
                if (u.relAt >= 0 && (S32)i >= u.relAt && (S32)i < u.relAt + 4) {
                    testPushCode8((stub - (offset + (U32)u.relAt + 4)) >> (8 * (i - u.relAt)));
                } else {
                    testPushCode8(u.b[i]);
                }
            }
            return offset + (U32)u.b.size();
        };
        U32 end = 0;
        for (const auto& u : units) end = place(end, u);
        t.code(end, {0xcd, 0x97});
        for (U32 i = 0; i < units.size(); ++i) t.code(place(perOp + i * stride, units[i]), {0xcd, 0x97});
        t.code(stub, {0xbf, 1, 0, 0, 0, 0xcd, 0x97}); // mov edi,1; int 97

        const U32 start[6] = {value(), value(), value(), value(), value(), value()}, startFlags = rnd() & flagMask;
        auto reset = [&]() {
            t.regs(start[0], start[1], start[2], start[3], start[4], start[5], 0);
            cpu->lazyFlagType = FLAGS_NONE;
            cpu->setFlags(startFlags, FMASK_ALL);
        };
        reset();
        t.run(t.compile(0), 0);
        U32 oneBlock[8];
        for (U32 i = 0; i < 8; ++i) oneBlock[i] = cpu->reg[i].u32;
        cpu->fillFlags();
        const U32 oneBlockFlags = cpu->flags & flagMask;

        reset();
        for (U32 i = 0; i < units.size() && !cpu->reg[7].u32; ++i) {
            const U32 offset = perOp + i * stride;
            t.run(t.compile(offset), offset);
        }
        cpu->fillFlags();
        bool same = (cpu->flags & flagMask) == oneBlockFlags;
        for (U32 i = 0; i < 8; ++i) same = same && cpu->reg[i].u32 == oneBlock[i];
        if (!same) {
            std::string hex;
            char text[8];
            for (const auto& u : units) {
                for (U8 x : u.b) { snprintf(text, sizeof(text), "%02x", x); hex += text; }
                hex += " ";
            }
            testFail("trial %u: one block flags=%x eax=%x ecx=%x edx=%x ebx=%x ebp=%x esi=%x edi=%x, per op flags=%x eax=%x ecx=%x edx=%x ebx=%x ebp=%x esi=%x edi=%x; start %x %x %x %x %x %x flags %x; code %s",
                trial, oneBlockFlags, oneBlock[0], oneBlock[1], oneBlock[2], oneBlock[3], oneBlock[5], oneBlock[6], oneBlock[7],
                cpu->flags & flagMask, cpu->reg[0].u32, cpu->reg[1].u32, cpu->reg[2].u32, cpu->reg[3].u32, cpu->reg[5].u32, cpu->reg[6].u32, cpu->reg[7].u32,
                start[0], start[1], start[2], start[3], start[4], start[5], startFlags, hex.c_str());
            break;
        }
        cpu->nextOp = nullptr;
    }
    contextTimeRemaining = savedBudget;
    testContext().cpu->nextOp = nullptr;
}

// Catches: the release-only stop before a block start (the exit in compileOps, the trim fix-up, a return site that is
// also the stop in emitInlinedCall) skipping or repeating the code at the stop, or leaving registers unpublished.
void testWasmJitStopBlockBefore() {
    extern S32 contextTimeRemaining;
    const S32 savedBudget = contextTimeRemaining;
    contextTimeRemaining = 0x7fffffff;
    JitShape t;
    CPU* cpu = t.cpu;
    enum { rEax, rEcx, rEdx, rEbx, rEsp, rEbp, rEsi, rEdi };
    struct StopBlocks {
        StopBlocks() { wasmJitTestSetStopBlocks(true); }
        ~StopBlocks() { wasmJitTestSetStopBlocks(false); }
    } stopBlocks;

    // An inlined call whose return site is already a block start. The block there reads eax from the CPU struct.
    t.rel32(0x00, {0xe8}, 0x100); // call 0x100
    t.code(0x05, {0x43, 0x89, 0xc2, 0xcd, 0x97}); // inc ebx; mov edx,eax; int 97
    t.code(0x100, {0xb8, 0x07, 0x00, 0x00, 0x00, 0x41, 0xc3}); // mov eax,7; inc ecx; ret
    cpu->getOp(TEST_CODE_ADDRESS + 0x100, 0); // inlining only copies code that was decoded
    DecodedOp* returnSite = t.compile(0x05);
    U64 inlinedBefore = wasmJitTestInlinedCalls();
    DecodedOp* caller = t.compile(0x00);
    if (wasmJitTestInlinedCalls() - inlinedBefore != 1) testFail("stop at a return site: the call must be inlined");
    if (caller->blockOpCount != 1) testFail("stop at a return site: block has %u ops", (U32)caller->blockOpCount);
    t.regs(0, 0, 0, 0, 0, 0, 0);
    t.run(caller, 0x00);
    if (cpu->reg[rEbx].u32 != 1 || cpu->reg[rEdx].u32 != 7 || cpu->reg[rEcx].u32 != 1 || cpu->reg[rEsp].u32 != t.stackTop) {
        testFail("stop at a return site: ebx=%u edx=%u ecx=%u esp=%x", cpu->reg[rEbx].u32, cpu->reg[rEdx].u32, cpu->reg[rEcx].u32, cpu->reg[rEsp].u32);
    }
    if (!returnSite->pfnJitCode) testFail("stop at a return site: the block there must stay");

    // A call that isn't inlined ends the block at its return site, compiled or not.
    t.rel32(0x20, {0xe8}, 0x140); // call 0x140
    t.code(0x25, {0x43, 0x89, 0xc2, 0xcd, 0x97}); // inc ebx; mov edx,eax; int 97
    t.code(0x140, {0xb8, 0x09, 0x00, 0x00, 0x00, 0x41, 0xc3}); // mov eax,9; inc ecx; ret
    setenv("BOXEDWINE_WASM_JIT_INLINE", "0", 1);
    caller = t.compile(0x20);
    unsetenv("BOXEDWINE_WASM_JIT_INLINE");
    if (caller->blockOpCount != 1) testFail("stop after a call: block has %u ops", (U32)caller->blockOpCount);
    t.regs(0, 0, 0, 0, 0, 0, 0);
    t.run(caller, 0x20);
    if (cpu->reg[rEbx].u32 != 1 || cpu->reg[rEdx].u32 != 9 || cpu->reg[rEcx].u32 != 1) {
        testFail("stop after a call: ebx=%u edx=%u ecx=%u", cpu->reg[rEbx].u32, cpu->reg[rEdx].u32, cpu->reg[rEcx].u32);
    }

    // A plain block running into a block start, with a jcc that jumps past that start: the trim must end on the op
    // before the stop, whichever path it took.
    // 240 test eax,eax; 242 jz 24a; 244 mov edi,5; 249 inc ebx (a block start); 24a inc esi; 24b mov ebp,edi; 24d int 97
    t.code(0x240, {0x85, 0xc0, 0x74, 0x06, 0xbf, 0x05, 0x00, 0x00, 0x00, 0x43, 0x46, 0x89, 0xfd, 0xcd, 0x97});
    DecodedOp* start = t.compile(0x249);
    caller = t.compile(0x240);
    if (caller->blockOpCount != 3) testFail("stop before a block start: block has %u ops", (U32)caller->blockOpCount);
    t.regs(1, 0, 0, 0, 0, 0, 0);
    t.run(caller, 0x240);
    if (cpu->reg[rEbx].u32 != 1 || cpu->reg[rEsi].u32 != 1 || cpu->reg[rEbp].u32 != 5) {
        testFail("stop before a block start, jcc not taken: ebx=%u esi=%u ebp=%u", cpu->reg[rEbx].u32, cpu->reg[rEsi].u32, cpu->reg[rEbp].u32);
    }
    t.regs(0, 0, 0, 0, 0, 0, 0);
    t.run(caller, 0x240);
    if (cpu->reg[rEbx].u32 != 0 || cpu->reg[rEsi].u32 != 1 || cpu->reg[rEbp].u32 != 0) {
        testFail("stop before a block start, jcc taken: ebx=%u esi=%u ebp=%u", cpu->reg[rEbx].u32, cpu->reg[rEsi].u32, cpu->reg[rEbp].u32);
    }
    if (!start->pfnJitCode) testFail("stop before a block start: the block there must stay");
    contextTimeRemaining = savedBudget;
    cpu->nextOp = nullptr;
}

// Catches: a change to fusing that breaks inlined code only when the callee sits below its caller (tryDirect stops at
// the caller's last op, so a callee above it never fuses); ret imm16's stack adjust; a jcc side exit past the inlined ret.
void testWasmJitInlineCalleeShapes() {
    extern S32 contextTimeRemaining;
    const S32 savedBudget = contextTimeRemaining;
    contextTimeRemaining = 0x7fffffff;
    JitShape t;
    CPU* cpu = t.cpu;
    enum { rEax, rEcx, rEdx, rEbx, rEsp, rEbp, rEsi, rEdi };
    auto decode = [&](std::initializer_list<U32> offsets) {
        for (U32 offset : offsets) cpu->getOp(TEST_CODE_ADDRESS + offset, 0);
    };

    // Callee below the caller, so cmp+sete fuses (the skipToOp branch), with a label on the consumer: test eax,eax; jz L; cmp ecx,edx; L: sete al; add esp,0; ret 4
    t.code(0x100, {0x85, 0xc0, 0x74, 0x02, 0x39, 0xd1, 0x0f, 0x94, 0xc0, 0x83, 0xc4, 0x00, 0xc2, 0x04, 0x00});
    t.code(0x300, {0x6a, 0x11}); // push 0x11
    t.rel32(0x302, {0xe8}, 0x100); // call 0x100
    t.code(0x307, {0xcd, 0x97});
    decode({0x100, 0x104, 0x106});
    U64 before = wasmJitTestInlinedCalls();
    DecodedOp* caller = t.compile(0x300);
    if (wasmJitTestInlinedCalls() - before != 1) testFail("inline shapes: the label callee must be inlined");
    struct Case { U32 eax, ecx, edx, expected; };
    for (const Case& c : {Case{0, 5, 6, 1}, Case{0x1200, 5, 5, 0x1201}, Case{0x1200, 5, 6, 0x1200}}) {
        t.regs(c.eax, c.ecx, c.edx, 0, 0, 0, 0);
        t.run(caller, 0x300);
        if (cpu->reg[rEax].u32 != c.expected || cpu->reg[rEsp].u32 != t.stackTop) {
            testFail("inline shapes, label: eax %x -> %x, expected %x, esp=%x", c.eax, cpu->reg[rEax].u32, c.expected, cpu->reg[rEsp].u32);
        }
    }

    // Callee below the caller, a cmp+jcc to a label of its own: inc ecx; cmp ecx,100; ja +3; add edx,2; ret
    t.code(0x140, {0x41, 0x83, 0xf9, 0x64, 0x77, 0x03, 0x83, 0xc2, 0x02, 0xc3});
    t.rel32(0x320, {0xe8}, 0x140); // call 0x140
    t.code(0x325, {0x43, 0xcd, 0x97}); // inc ebx; int 97
    decode({0x140, 0x146, 0x149});
    before = wasmJitTestInlinedCalls();
    caller = t.compile(0x320);
    if (wasmJitTestInlinedCalls() - before != 1) testFail("inline shapes: the jcc callee must be inlined");
    t.regs(0, 99, 0, 0, 0, 0, 0);
    t.run(caller, 0x320);
    if (cpu->reg[rEdx].u32 != 2 || cpu->reg[rEbx].u32 != 1 || cpu->reg[rEsp].u32 != t.stackTop) {
        testFail("inline shapes, jcc not taken: edx=%u ebx=%u", cpu->reg[rEdx].u32, cpu->reg[rEbx].u32);
    }
    t.regs(0, 100, 0, 0, 0, 0, 0);
    t.run(caller, 0x320);
    if (cpu->reg[rEdx].u32 != 0 || cpu->reg[rEbx].u32 != 1 || cpu->reg[rEsp].u32 != t.stackTop) {
        testFail("inline shapes, jcc taken: edx=%u ebx=%u", cpu->reg[rEdx].u32, cpu->reg[rEbx].u32);
    }

    // A jcc side exit past the inlined ret, with the return address still on the stack and a dirty caller register.
    t.code(0x180, {0x41, 0x39, 0xd1}); // inc ecx; cmp ecx,edx
    t.rel32(0x183, {0x0f, 0x87}, 0x1c0); // ja 0x1c0
    t.code(0x189, {0xc3}); // ret
    t.code(0x1c0, {0x46, 0x89, 0xdd, 0xcd, 0x97}); // inc esi; mov ebp,ebx; int 97
    t.code(0x340, {0xbb, 0x05, 0x00, 0x00, 0x00}); // mov ebx,5
    t.rel32(0x345, {0xe8}, 0x180); // call 0x180
    t.code(0x34a, {0x47, 0xcd, 0x97}); // inc edi; int 97
    decode({0x180, 0x189, 0x1c0});
    before = wasmJitTestInlinedCalls();
    caller = t.compile(0x340);
    if (wasmJitTestInlinedCalls() - before != 1) testFail("inline shapes: the side exit callee must be inlined");
    t.regs(0, 9, 0, 0, 0, 0, 0);
    t.run(caller, 0x340);
    if (cpu->reg[rEsi].u32 != 1 || cpu->reg[rEbp].u32 != 5 || cpu->reg[rEdi].u32 != 0 || cpu->reg[rEcx].u32 != 10 || cpu->reg[rEsp].u32 != t.stackTop - 4) {
        testFail("inline shapes, side exit: esi=%u ebp=%u edi=%u ecx=%u esp=%x", cpu->reg[rEsi].u32, cpu->reg[rEbp].u32, cpu->reg[rEdi].u32, cpu->reg[rEcx].u32, cpu->reg[rEsp].u32);
    }
    t.regs(0, 0, 9, 0, 0, 0, 0);
    t.run(caller, 0x340);
    if (cpu->reg[rEsi].u32 != 0 || cpu->reg[rEdi].u32 != 1 || cpu->reg[rEsp].u32 != t.stackTop) {
        testFail("inline shapes, past the jcc: esi=%u edi=%u esp=%x", cpu->reg[rEsi].u32, cpu->reg[rEdi].u32, cpu->reg[rEsp].u32);
    }
    contextTimeRemaining = savedBudget;
    cpu->nextOp = nullptr;
}

// Catches: one rejected function (or a module over a browser's size limit, which is refused the same way) sending a
// whole group to the interpreter for good, a failed hot group losing the code its blocks already have, and a block the
// browser rejects being compiled again every period.
void testWasmJitFailedGroupSplits() {
    testNewInstruction(0);
    TestContext& context = testContext();
    CPU* cpu = context.cpu;
    constexpr U32 blockCount = 8;
    constexpr U32 blockBytes = 8;
    constexpr U32 hotEntries = 4;
    wasmJitTestResetRuntimeBatching();
    struct Reset {
        ~Reset() {
            wasmJitTestFailBatchAfterSlots(-1);
            wasmJitTestResetRuntimeBatching();
        }
    } reset;
    wasmJitTestSetHotGroups(hotEntries, 1e12); // entries per period; the period never ends here: flush below
    for (U32 i = 0; i < blockCount; ++i) {
        testPushCode8(0x43); // inc ebx
        testPushCode8(0xb8); testPushCode32((i + 1) * blockBytes); // mov eax,next
        testPushCode8(0xff); testPushCode8(0xe0); // jmp eax: separate blocks
    }
    testPushCode8(0xcd); testPushCode8(0x97);
    std::vector<DecodedOp*> blocks;
    std::vector<void*> tier1;
    for (U32 i = 0; i < blockCount; ++i) {
        U32 address = TEST_CODE_ADDRESS + i * blockBytes;
        DecodedOp* op = cpu->getOp(address, 0);
        startNewJIT(cpu, address, op);
        op->runCount = JIT_RUN_COUNT + 1;
        blocks.push_back(op);
        tier1.push_back(op->pfnJitCode);
    }
    auto runOnce = [&]() {
        cpu->eip.u32 = 0;
        cpu->reg[3].u32 = 0;
        cpu->nextOp = blocks[0];
        cpu->yield = false;
        for (U32 i = 0; i < 4 * blockCount && cpu->nextOp->inst != TestEnd; ++i) {
            cpu->blockInstructionCount = 0;
            cpu->run();
        }
        return cpu->reg[3].u32 == blockCount && cpu->nextOp->inst == TestEnd;
    };
    auto queueAll = [&]() {
        for (U32 i = 0; i < hotEntries; ++i) {
            if (!runOnce()) testFail("failed group: blocks must run the chain");
        }
        wasmJitTestClearHotCounts();
        if (wasmJitTestHotQueueSize() != blockCount) testFail("failed group: every block must queue");
    };

    // Every attempt fails: each block keeps the code it has, none goes to the interpreter.
    queueAll();
    wasmJitTestFailBatchAfterSlots(0);
    wasmJitTestHotFlush(cpu);
    wasmJitTestFailBatchAfterSlots(-1);
    for (U32 i = 0; i < blockCount; ++i) {
        DecodedOp* op = blocks[i];
        if (op->pfnJitCode != tier1[i] || !(op->flags & OP_FLAG_JIT) || op->pfn != context.process->startJITOp || (op->flags2 & OP_FLAG2_WASM_JIT_PENDING)) {
            testFail("failed hot group: block %u must keep its old code", i);
        }
    }
    if (!runOnce() || wasmJitCompilationPaused() || wasmJitTestPendingCount() != 0) testFail("failed hot group: the guest must keep running");

    // Rejected for good: the blocks get hot again but must not be compiled again.
    for (U32 i = 0; i < hotEntries; ++i) {
        if (!runOnce()) testFail("rejected hot group: blocks must run the chain");
    }
    wasmJitTestClearHotCounts();
    if (wasmJitTestHotQueueSize() != 0) testFail("rejected hot group: %u blocks queued again", wasmJitTestHotQueueSize());
    wasmJitTestForgetHotRejections(); // the next part reuses these blocks

    // Too big as one (more than 5 slots fail here, as a module over a size limit does): the halves install.
    queueAll();
    wasmJitTestFailBatchAfterSlots(5);
    wasmJitTestHotFlush(cpu);
    wasmJitTestFailBatchAfterSlots(-1);
    if (!runOnce()) testFail("split hot group: the guest must keep running");
    for (U32 i = 0; i < blockCount; ++i) {
        if (!blocks[i]->pfnJitCode || blocks[i]->pfnJitCode == tier1[i]) {
            testFail("split hot group: block %u must be published at a new slot", i);
        }
    }
    if (wasmJitTestPendingCount() != 0) testFail("split hot group: nothing stays pending");
    cpu->nextOp = nullptr;
    for (U32 i = 0; i < blockCount; ++i) {
        context.memory->removeCodeBlock(TEST_CODE_ADDRESS + i * blockBytes, blocks[i], false);
    }
    wasmJitTestResetRuntimeBatching();

    // A file batch with one block the browser rejects: only that block is given up on.
    WasmJitBatchLimits limits;
    limits.maxBlocks = 4;
    limits.maxBatchBytes = 1024 * 1024;
    limits.urgentPendingHits = 1000;
    limits.maxProcessOpenBytes = 4 * 1024 * 1024;
    wasmJitTestSetBatchLimits(limits);
    wasmJitTestSetMappedFileKeyOverride(79);
    wasmJitTestEnableRuntimeBatching(true);
    context.codeIp = TEST_CODE_ADDRESS + 0x100;
    DecodedOp* ops[4];
    U32 addresses[4];
    for (U32 i = 0; i < 4; ++i) {
        addresses[i] = context.codeIp;
        testPushCode8(0x40 + i); // inc eax, ecx, edx, ebx
        testPushCode8(0xcd); testPushCode8(0x97);
    }
    WasmJitRuntimeStatsSnapshot before = wasmJitTestGetRuntimeStats();
    for (U32 i = 0; i < 4; ++i) {
        ops[i] = cpu->getOp(addresses[i], 0);
        startNewJIT(cpu, addresses[i], ops[i]);
        if (i == 1) wasmJitTestCorruptPendingBlock(ops[i]);
    }
    WasmJitRuntimeStatsSnapshot after = wasmJitTestGetRuntimeStats();
    if (after.permanentFailures - before.permanentFailures != 1) {
        testFail("split file batch: %llu blocks given up on, expected 1", (unsigned long long)(after.permanentFailures - before.permanentFailures));
    }
    if (wasmJitTestPendingCount() != 0) testFail("split file batch: nothing stays pending");
    for (U32 i = 0; i < 4; ++i) {
        if ((ops[i]->pfnJitCode != nullptr) != (i != 1)) testFail("split file batch: block %u has the wrong outcome", i);
    }
    for (U32 i = 0; i < 4; ++i) {
        if (ops[i]->pfnJitCode) context.memory->removeCodeBlock(addresses[i], ops[i], false);
    }
}

// Batches compile off the page thread (WebAssembly.compile) and install at a later main loop tick. Catches: a hot
// block losing its old code while its hot group compiles, a block whose code is invalidated meanwhile (a write, an
// unmap) installed anyway, and a fresh block not left to the interpreter until its batch installs.
void testWasmJitAsyncInstall() {
    testNewInstruction(0);
    TestContext& context = testContext();
    CPU* cpu = context.cpu;
    constexpr U32 blockCount = 4;
    constexpr U32 blockBytes = 8;
    constexpr U32 hotEntries = 4;
    wasmJitTestResetRuntimeBatching();
    struct Reset {
        ~Reset() { wasmJitTestResetRuntimeBatching(); }
    } reset;
    wasmJitTestSetHotGroups(hotEntries, 1e12); // entries per period; the period never ends here: flush below
    // Separate blocks that each end the test: a hot block would follow a jump into the next block's code and cover
    // it too. So each covers only its own code, and invalidating one block cancels only its own recompile.
    for (U32 i = 0; i < blockCount; ++i) {
        testPushCode8(0x43); // inc ebx
        testPushCode8(0xcd); testPushCode8(0x97); // TestEnd
        for (U32 pad = 3; pad < blockBytes; ++pad) testPushCode8(0x90);
    }
    std::vector<DecodedOp*> blocks;
    std::vector<void*> tier1;
    for (U32 i = 0; i < blockCount; ++i) {
        U32 address = TEST_CODE_ADDRESS + i * blockBytes;
        DecodedOp* op = cpu->getOp(address, 0);
        startNewJIT(cpu, address, op);
        op->runCount = JIT_RUN_COUNT + 1;
        blocks.push_back(op);
        tier1.push_back(op->pfnJitCode);
    }
    auto runOnce = [&]() {
        cpu->reg[3].u32 = 0;
        for (U32 b = 0; b < blockCount; ++b) {
            cpu->eip.u32 = b * blockBytes;
            cpu->nextOp = blocks[b];
            cpu->yield = false;
            for (U32 i = 0; i < 4 && cpu->nextOp->inst != TestEnd; ++i) {
                cpu->blockInstructionCount = 0;
                cpu->run();
            }
            if (cpu->nextOp->inst != TestEnd) return false;
        }
        return cpu->reg[3].u32 == blockCount;
    };
    for (U32 i = 0; i < hotEntries; ++i) {
        if (!runOnce()) testFail("async install: blocks must run the chain");
    }
    wasmJitTestClearHotCounts();
    if (wasmJitTestHotQueueSize() != blockCount) testFail("async install: every block must queue");

    wasmJitTestDeferInstall(true);
    wasmJitTestHotFlush(cpu);
    for (U32 i = 0; i < blockCount; ++i) {
        DecodedOp* op = blocks[i];
        if (op->pfnJitCode != tier1[i] || !(op->flags & OP_FLAG_JIT) || op->pfn != context.process->startJITOp ||
                (op->flags2 & OP_FLAG2_WASM_JIT_PENDING) || op->runCount != JIT_RUN_COUNT + 1) {
            testFail("async hot group: block %u must keep its old code while the group compiles", i);
        }
        if (op->blockStart != op) testFail("async hot group: block %u must cover only its own code", i);
    }
    if (wasmJitTestPendingCount() != blockCount) testFail("async hot group: every block must wait for the install");
    if (!runOnce()) testFail("async hot group: the old code must run while the group compiles");
    // The last block's code is invalidated while the group compiles: it must not install.
    DecodedOp* dropped = blocks[blockCount - 1];
    context.memory->removeCodeBlock(TEST_CODE_ADDRESS + (blockCount - 1) * blockBytes, dropped, false);
    if (wasmJitTestPendingCount() != blockCount - 1) testFail("async hot group: invalidated code must cancel its recompile");
    wasmJitTestTick();
    for (U32 i = 0; i < blockCount - 1; ++i) {
        if (!blocks[i]->pfnJitCode || blocks[i]->pfnJitCode == tier1[i] || blocks[i]->pfn != context.process->startJITOp) {
            testFail("async hot group: block %u must be published at its new slot at the tick", i);
        }
    }
    if (dropped->pfnJitCode || wasmJitTestPendingCount() != 0 || wasmJitTestRuntimeGroupCount() != 1) {
        testFail("async hot group: the invalidated block must stay uninstalled, and nothing pending");
    }
    if (!runOnce()) testFail("async hot group: the group must run after the install");
    cpu->nextOp = nullptr;
    for (U32 i = 0; i < blockCount; ++i) {
        if (blocks[i]->pfnJitCode) context.memory->removeCodeBlock(TEST_CODE_ADDRESS + i * blockBytes, blocks[i], false);
    }
    wasmJitTestResetRuntimeBatching();

    // Fresh blocks run in the interpreter until their batch installs; one invalidated meanwhile never installs.
    WasmJitBatchLimits limits;
    limits.maxBlocks = 2;
    limits.maxBatchBytes = 1024 * 1024;
    limits.urgentPendingHits = 1000;
    limits.maxProcessOpenBytes = 4 * 1024 * 1024;
    wasmJitTestSetBatchLimits(limits);
    wasmJitTestSetMappedFileKeyOverride(79);
    wasmJitTestEnableRuntimeBatching(true);
    wasmJitTestDeferInstall(true);
    context.codeIp = TEST_CODE_ADDRESS + 0x100;
    DecodedOp* ops[2];
    U32 addresses[2];
    for (U32 i = 0; i < 2; ++i) {
        addresses[i] = context.codeIp;
        testPushCode8(0x40 + i); // inc eax, ecx
        testPushCode8(0xcd); testPushCode8(0x97);
    }
    for (U32 i = 0; i < 2; ++i) {
        ops[i] = cpu->getOp(addresses[i], 0);
        startNewJIT(cpu, addresses[i], ops[i]);
    }
    for (U32 i = 0; i < 2; ++i) {
        if (ops[i]->pfnJitCode || !(ops[i]->flags2 & OP_FLAG2_WASM_JIT_PENDING)) {
            testFail("async file batch: block %u must stay pending while its batch compiles", i);
        }
    }
    if (wasmJitTestPendingCount() != 2) testFail("async file batch: both blocks must wait for the install");
    context.memory->removeCodeBlock(addresses[0], ops[0], false);
    if (wasmJitTestPendingCount() != 1) testFail("async file batch: invalidated code must cancel its pending block");
    wasmJitTestTick();
    if (ops[0]->pfnJitCode || !ops[1]->pfnJitCode || (ops[1]->flags2 & OP_FLAG2_WASM_JIT_PENDING) || wasmJitTestPendingCount() != 0) {
        testFail("async file batch: only the live block must install at the tick");
    }
    for (U32 i = 0; i < 2; ++i) {
        if (ops[i]->pfnJitCode) context.memory->removeCodeBlock(addresses[i], ops[i], false);
    }

    // A failed compile is only seen at the tick that installs it. Catches: its blocks stuck pending (Compiling) for
    // good, or not given the recovery a failure got before (halves tried again, "out of memory" sealed for a retry).
    boxedwine_wasm_test_reset_oom_state();
    struct ResetOom {
        ~ResetOom() { boxedwine_wasm_test_reset_oom_state(); }
    } resetOom;
    auto startBatch = [&](U32 base, bool forceOom) {
        wasmJitTestResetRuntimeBatching();
        wasmJitTestSetBatchLimits(limits);
        wasmJitTestSetMappedFileKeyOverride(79);
        wasmJitTestEnableRuntimeBatching(true);
        wasmJitTestDeferInstall(true);
        if (forceOom) boxedwine_wasm_test_force_next_module_oom();
        context.codeIp = TEST_CODE_ADDRESS + base;
        for (U32 i = 0; i < 2; ++i) {
            addresses[i] = context.codeIp;
            testPushCode8(0x40 + i); // inc eax, ecx
            testPushCode8(0xcd); testPushCode8(0x97);
        }
        for (U32 i = 0; i < 2; ++i) {
            ops[i] = cpu->getOp(addresses[i], 0);
            startNewJIT(cpu, addresses[i], ops[i]);
        }
        if (wasmJitTestPendingCount() != 2) testFail("async failure: both blocks must wait for the install");
    };

    // The module is rejected (here every slot fails): the halves compile again and fail at later ticks, and each
    // block is left to the interpreter.
    startBatch(0x200, false);
    wasmJitTestFailBatchAfterSlots(0);
    for (U32 tick = 0; tick < 4 && wasmJitTestPendingCount(); ++tick) {
        wasmJitTestTick();
    }
    wasmJitTestFailBatchAfterSlots(-1);
    if (wasmJitTestPendingCount() != 0) testFail("async rejected batch: %u blocks stay pending", wasmJitTestPendingCount());
    for (U32 i = 0; i < 2; ++i) {
        if (ops[i]->pfnJitCode || (ops[i]->flags & OP_FLAG_JIT) || (ops[i]->flags2 & OP_FLAG2_WASM_JIT_PENDING) ||
                ops[i]->pfn == context.process->startJITOp) {
            testFail("async rejected batch: block %u must be left to the interpreter", i);
        }
    }

    // "Out of memory" from the compile: the blocks wait sealed, in the interpreter, and install after the retry.
    startBatch(0x300, true);
    boxedwine_wasm_test_clear_forced_module_oom();
    wasmJitTestTick();
    if (wasmJitTestPendingCount() != 2 || wasmJitTestSealedCount() != 1 || !wasmJitCompilationPaused()) {
        testFail("async out of memory: both blocks must wait sealed, and compiling is paused");
    }
    for (U32 i = 0; i < 2; ++i) {
        if (ops[i]->pfnJitCode || !(ops[i]->flags2 & OP_FLAG2_WASM_JIT_PENDING)) {
            testFail("async out of memory: block %u must stay pending in the interpreter", i);
        }
    }
    // That tick also armed the retry at its own time plus 5 s; the retry starts the compile again.
    wasmJitTestOomTick(emscripten_get_now() + 5000);
    wasmJitTestTick();
    if (wasmJitTestPendingCount() != 0 || wasmJitTestSealedCount() != 0 || wasmJitCompilationPaused()) {
        testFail("async out of memory: the retry must install the batch");
    }
    for (U32 i = 0; i < 2; ++i) {
        if (!ops[i]->pfnJitCode || ops[i]->pfn != context.process->startJITOp) {
            testFail("async out of memory: block %u must be installed after the retry", i);
        }
        if (ops[i]->pfnJitCode) context.memory->removeCodeBlock(addresses[i], ops[i], false);
    }
}
#endif

#ifdef BOXEDWINE_MULTI_THREADED
void testWasmJitMtBoundedDispatch() {
    testNewInstruction(0);
    TestContext& context = testContext();
    CPU* cpu = context.cpu;
    constexpr U32 blockCount = 96;
    constexpr U32 blockBytes = 8;
    std::vector<DecodedOp*> blocks;
    for (U32 i = 0; i < blockCount; i++) {
        testPushCode8(0x43); // inc ebx
        testPushCode8(0xb8); testPushCode32((i + 1) * blockBytes); // mov eax,next
        testPushCode8(0xff); testPushCode8(0xe0); // jmp eax: separate JIT blocks
    }
    testPushCode8(0xcd); testPushCode8(0x97);
    for (U32 i = 0; i < blockCount; i++) {
        U32 address = TEST_CODE_ADDRESS + i * blockBytes;
        DecodedOp* op = cpu->getOp(address, 0);
        startNewJIT(cpu, address, op);
        if (!op->pfnJitCode || op->pfn != context.process->startJITOp) {
            testFail("bounded dispatch requires compiled block entries");
            return;
        }
        op->runCount = JIT_RUN_COUNT + 1;
        blocks.push_back(op);
    }
    auto restart = [&]() {
        cpu->eip.u32 = 0;
        cpu->reg[3].u32 = 0;
        cpu->nextOp = blocks[0];
    };
    restart();
    cpu->run();
    if (cpu->reg[3].u32 <= 1 || cpu->reg[3].u32 >= blockCount) {
        testFail("MT dispatch must chain compiled blocks but return at a finite bound");
    }
    for (U32 i = 0; i < blockCount && (!cpu->nextOp || cpu->nextOp->inst != TestEnd); i++) {
        cpu->run();
    }
    if (cpu->reg[3].u32 != blockCount || !cpu->nextOp || cpu->nextOp->inst != TestEnd) {
        testFail("bounded dispatch must execute every guest block exactly once");
    }

    // Enter the compiled dispatcher directly so each stop condition is
    // observed at the new boundary after the first guest block executes.
    for (U32 stop = 0; stop < 7; stop++) {
        restart();
        switch (stop) {
        case 0: cpu->yield = true; break;
        case 1: context.thread->terminating = true; break;
        case 2: context.process->terminated = true; break;
        case 3: cpu->debugTrapActive = true; break;
        case 4: context.thread->ptraceStopped.store(true, std::memory_order_release); break;
        case 5: cpu->jitSignalPending.store(1, std::memory_order_release); break;
        case 6: context.thread->pendingSignals = 1; break;
        }
        context.process->startJITOp(cpu, blocks[0]);
        U32 executed = cpu->reg[3].u32;
        cpu->yield = false;
        context.thread->terminating = false;
        context.process->terminated = false;
        cpu->debugTrapActive = false;
        context.thread->ptraceStopped.store(false, std::memory_order_release);
        cpu->jitSignalPending.store(0, std::memory_order_release);
        context.thread->pendingSignals = 0;
        if (executed != 1) {
            testFail("bounded dispatch ignored stop condition %u: executed %u blocks", stop, executed);
        }
    }
    cpu->nextOp = nullptr;
}

void testWasmJitMtCpuHazardStateIsCold() {
    CPU* cpu = testContext().cpu;
    uintptr_t existingStateEnd = (uintptr_t)&cpu->big + sizeof(cpu->big) - 1;
    uintptr_t hazardStateStart = (uintptr_t)&cpu->wasmJitActiveTableIndex;
    if (hazardStateStart <= existingStateEnd) {
        testFail("MT WASM JIT owner-hazard bookkeeping must not shift existing CPU/JIT state");
    }
}

void testWasmJitMtExecDetachPreservesSharedDecodedOps() {
    testNewInstruction(0);
    U32 address = testContext().codeIp;
    testPushCode8(0x90); // nop
    testPushCode8(0xcd);
    testPushCode8(0x97);
    DecodedOp* sharedOp = testContext().cpu->getOp(address, 0);
    void* sentinelJitCode = reinterpret_cast<void*>(0x1234);
    sharedOp->pfnJitCode = sentinelJitCode;

    KProcessPtr cloneProcess = KProcess::create();
    KMemory* cloneMemory = KMemory::create(cloneProcess.get());
    cloneProcess->memory = cloneMemory;
    cloneMemory->clone(testContext().memory, true);
    cloneMemory->execvReset(true);

    if (sharedOp->pfnJitCode != sentinelJitCode) {
        testFail("MT WASM JIT CLONE_VM exec detach must not sweep the shared decoded-op cache");
    }
    sharedOp->pfnJitCode = nullptr;

    U32 cloneProcessId = cloneProcess->id;
    KProcessWeakPtr weakCloneProcess = cloneProcess;
    if (KSystem::getProcess(cloneProcessId)) {
        KSystem::eraseProcess(cloneProcessId);
    }
    cloneProcess.reset();
    if (!weakCloneProcess.expired()) {
        testFail("MT WASM JIT exec detach test leaked its clone process");
    }
}
#endif

static std::vector<U8> makeMergeInput(S32 value, const char* memoryName = "memory", const char* profileName = nullptr) {
    WasmEmitter emitter;
    U32 type = emitter.addFuncType({WasmType::I32, WasmType::I32}, {});
    emitter.addMemoryImport("env", memoryName);
    if (profileName) {
        U32 imported = emitter.addFunctionImport("env", "profile_noop", type);
        emitter.setFunctionName(imported, "imported_helper");
    }
    U32 function = emitter.addFunction(type);
    emitter.addExport("execute", function);
    if (profileName) {
        emitter.setFunctionName(function, profileName);
    }
    emitter.beginFunction({});
    emitter.emitLocalGet(0);
    emitter.emitI32Const(value);
    emitter.emitI32Store(0);
    emitter.endFunction();
    return emitter.finalize();
}

EM_JS(int, test_run_merged_wasm, (const void* bytes, int size, int count, U32 output), {
    try {
        var module = new WebAssembly.Module(new Uint8Array(HEAPU8.buffer, bytes, size));
        var instance = new WebAssembly.Instance(module, {env: {memory: wasmMemory, profile_noop: function() {}}});
        for (var i = 0; i < count; ++i) {
            var fn = instance.exports['b' + i];
            if (!fn) {
                return 0;
            }
            fn(output + i * 4, 0);
        }
        return 1;
    } catch (e) {
        console.error('test_run_merged_wasm failed:', e);
        return 0;
    }
});

EM_JS(int, test_merged_wasm_names, (const void* bytes, int size, const char* first, const char* second), {
    var module = new WebAssembly.Module(new Uint8Array(HEAPU8.buffer, bytes, size));
    var sections = WebAssembly.Module.customSections(module, 'name');
    if (sections.length !== 1) return 0;
    var data = new Uint8Array(sections[0]);
    var pos = 0;
    function uleb() {
        var result = 0, shift = 0, byte;
        do {
            if (pos >= data.length || shift >= 35) throw new Error('bad name ULEB');
            byte = data[pos++];
            result |= (byte & 127) << shift;
            shift += 7;
        } while (byte & 128);
        return result;
    }
    if (data[pos++] !== 1) return 0;
    var subsectionSize = uleb();
    if (subsectionSize !== data.length - pos || uleb() !== 2) return 0;
    var expected = [UTF8ToString(first), UTF8ToString(second)];
    for (var i = 0; i < 2; ++i) {
        // One shared function import precedes the two defined functions.
        if (uleb() !== i + 1) return 0;
        var length = uleb();
        var name = new TextDecoder().decode(data.subarray(pos, pos + length));
        pos += length;
        if (name !== expected[i]) return 0;
    }
    return pos === data.length ? 1 : 0;
});

void testWasmJitModuleMerger() {
    std::vector<U8> first = makeMergeInput(0x11223344);
    std::vector<U8> second = makeMergeInput(0x55667788);
    std::vector<WasmJitMergeInput> inputs = {{&first}, {&second}};
    std::vector<U8> merged;
    BString error;
    if (!wasmJitMergeModules(inputs, merged, error)) {
        testFail("wasm runtime merger failed: %s", error.c_str());
        return;
    }
    U32 output[2] = {};
    if (!test_run_merged_wasm(merged.data(), (int)merged.size(), 2, (U32)(uintptr_t)output)) {
        testFail("wasm runtime merger browser validation");
    }
    if (output[0] != 0x11223344 || output[1] != 0x55667788) {
        testFail("wasm runtime merger exports execute distinct bodies");
    }

    const char* firstName = "x86:test:file_00001000:pc_00401000";
    std::string secondName = "x86:" + std::string(140, 'x') + ":pc_00402000";
    std::vector<U8> namedFirst = makeMergeInput(0x11223344, "memory", firstName);
    std::vector<U8> namedSecond = makeMergeInput(0x55667788, "memory", secondName.c_str());
    inputs = {{&namedFirst}, {&namedSecond}};
    if (!wasmJitMergeModules(inputs, merged, error) ||
        !test_merged_wasm_names(merged.data(), (int)merged.size(), firstName, secondName.c_str()) ||
        !test_run_merged_wasm(merged.data(), (int)merged.size(), 2, (U32)(uintptr_t)output) ||
        output[0] != 0x11223344 || output[1] != 0x55667788) {
        testFail("wasm runtime merger preserves function names with remapped indices and executable bodies");
    }

    std::vector<U8> incompatible = makeMergeInput(3, "otherMemory");
    inputs = {{&first}, {&incompatible}};
    if (wasmJitMergeModules(inputs, merged, error)) {
        testFail("wasm runtime merger rejects ABI mismatch");
    }
    std::vector<U8> truncated(first.begin(), first.end() - 1);
    inputs = {{&truncated}};
    if (wasmJitMergeModules(inputs, merged, error)) {
        testFail("wasm runtime merger rejects truncated code section");
    }
    inputs.clear();
    if (wasmJitMergeModules(inputs, merged, error)) {
        testFail("wasm runtime merger rejects empty input");
    }
    std::vector<U8> headerOnly = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
    inputs = {{&headerOnly}};
    if (wasmJitMergeModules(inputs, merged, error)) {
        testFail("wasm runtime merger rejects missing sections");
    }
    std::vector<U8> malformedUleb = headerOnly;
    malformedUleb.insert(malformedUleb.end(), {1, 0x80, 0x80, 0x80, 0x80, 0x10});
    inputs = {{&malformedUleb}};
    if (wasmJitMergeModules(inputs, merged, error)) {
        testFail("wasm runtime merger rejects overflowing section size");
    }
    std::vector<U8> alreadyMerged;
    inputs = {{&first}, {&second}};
    if (!wasmJitMergeModules(inputs, alreadyMerged, error)) {
        testFail("wasm runtime merger setup for multi-function rejection");
    } else {
        inputs = {{&alreadyMerged}};
        if (wasmJitMergeModules(inputs, merged, error)) {
            testFail("wasm runtime merger rejects multi-function input");
        }
    }
}

void testWasmJitBatchPolicy() {
    WasmJitBatchLimits limits;
    limits.maxBlocks = 2;
    limits.maxBatchBytes = 10;
    limits.urgentPendingHits = 2;
    limits.maxProcessOpenBytes = 12;
    KMemory* mem1 = reinterpret_cast<KMemory*>(0x1000);
    KMemory* mem2 = reinterpret_cast<KMemory*>(0x2000);
    WasmJitBatchKey a{mem1, 7};
    WasmJitBatchKey b{mem1, 8};

    WasmJitBatchPolicy countPolicy(limits);
    if (!countPolicy.enqueue(1, a, 4).empty()) {
        testFail("batch count early flush");
    }
    auto countFlush = countPolicy.enqueue(2, a, 4);
    if (countFlush.size() != 1 || countFlush[0].reason != WasmJitFlushReason::BlockCount || countFlush[0].entries != std::vector<U64>({1, 2})) {
        testFail("batch count flush contents");
    }

    WasmJitBatchPolicy bytePolicy(limits);
    bytePolicy.enqueue(3, a, 8);
    auto byteFlush = bytePolicy.enqueue(4, a, 4);
    if (byteFlush.size() != 1 || byteFlush[0].reason != WasmJitFlushReason::ByteCount || byteFlush[0].entries != std::vector<U64>({3})) {
        testFail("batch byte flush before overflow entry");
    }

    WasmJitBatchPolicy hitPolicy(limits);
    hitPolicy.enqueue(5, a, 4);
    WasmJitFlushRequest hitFlush;
    if (hitPolicy.recordPendingHit(5, hitFlush)) {
        testFail("batch urgent early flush");
    }
    if (!hitPolicy.recordPendingHit(5, hitFlush) || hitFlush.reason != WasmJitFlushReason::PendingHits || hitFlush.entries != std::vector<U64>({5})) {
        testFail("batch urgent hit flush");
    }

    WasmJitBatchPolicy productionHitPolicy;
    productionHitPolicy.enqueue(50, a, 4);
    WasmJitFlushRequest productionHitFlush;
    for (U32 i = 0; i < 63; ++i) {
        if (productionHitPolicy.recordPendingHit(50, productionHitFlush)) {
            testFail("batch production urgent flush before hit 64");
            break;
        }
    }
    if (!productionHitPolicy.recordPendingHit(50, productionHitFlush) ||
            productionHitFlush.reason != WasmJitFlushReason::PendingHits ||
            productionHitFlush.entries != std::vector<U64>({50})) {
        testFail("batch production urgent flush on hit 64");
    }

    WasmJitBatchPolicy capPolicy(limits);
    capPolicy.enqueue(6, a, 8);
    auto capFlush = capPolicy.enqueue(7, b, 8);
    if (capFlush.size() != 1 || capFlush[0].reason != WasmJitFlushReason::ProcessBytes || capFlush[0].entries != std::vector<U64>({6}) || capPolicy.openBytesForMemory(mem1) != 8) {
        testFail("batch process cap flushes oldest mapping");
    }
    capPolicy.enqueue(8, {mem2, 7}, 4);
    if (!capPolicy.cancel(7) || capPolicy.cancel(999)) {
        testFail("batch cancel result");
    }
    auto cancelled = capPolicy.cancelMemory(mem2);
    if (cancelled != std::vector<U64>({8})) {
        testFail("batch cancel memory entries");
    }

    WasmJitBatchPolicy processPolicy(limits);
    if (!processPolicy.enqueue(9, {mem1, 55}, 4).empty() || !processPolicy.enqueue(10, {mem2, 55}, 4).empty()) {
        testFail("batch same mapping key stays isolated by process memory");
    }
    if (processPolicy.openEntryCount() != 2) {
        testFail("batch process isolation keeps two open entries");
    }
}

void testWasmJitMappedFileRange() {
    auto oldMap = std::make_shared<MappedFile>();
    oldMap->address = 0x1000;
    oldMap->len = 0x1000;
    oldMap->key = 10;
    auto newMap = std::make_shared<MappedFile>();
    newMap->address = 0x1000;
    newMap->len = 0x1000;
    newMap->key = 20;
    auto other = std::make_shared<MappedFile>();
    other->address = 0x3000;
    other->len = 0x1000;
    other->key = 30;
    auto endMap = std::make_shared<MappedFile>();
    endMap->address = 0xfffff000;
    endMap->len = 0x1000;
    endMap->key = 40;
    std::vector<MappedFilePtr> maps = {oldMap, other, newMap, endMap};

    if (KProcess::selectMappedFileForRangeForTest(maps, 0x1100, 0x20) != newMap) {
        testFail("mapped range chooses newest overlapping mapping");
    }
    if (KProcess::selectMappedFileForRangeForTest(maps, 0x1ff0, 0x20)) {
        testFail("mapped range rejects crossing mapping end");
    }
    if (KProcess::selectMappedFileForRangeForTest(maps, 0x1100, 0)) {
        testFail("mapped range rejects empty block");
    }
    if (KProcess::selectMappedFileForRangeForTest(maps, 0xfffffff0, 0x40)) {
        testFail("mapped range rejects address overflow");
    }
    if (KProcess::selectMappedFileForRangeForTest(maps, 0xfffffff0, 0x10) != endMap) {
        testFail("mapped range accepts end at 4 GiB");
    }
}

#ifdef BOXEDWINE_MULTI_THREADED
EM_JS(S32, testWasmJitMtRuntimeGroupSlotIsClear, (S32 slot), {
    return slot >= 0 && slot < wasmTable.length && wasmTable.get(slot) === null;
});

EM_JS(S32, testWasmJitMtCallRuntimeGroupSlot, (S32 slot, S32* destination), {
    HEAP32[destination >> 2] = 0;
    if (slot < 0 || slot >= wasmTable.length) {
        return 0;
    }
    var fn = wasmTable.get(slot);
    if (!fn) {
        return 0;
    }
    fn(destination, 0);
    return 1;
});

EM_JS(U32, testWasmJitMtOomClassifierExclusions, (), {
    if (typeof globalThis.bwWasmJitBrokerIsOom !== 'function') {
        return 0;
    }
    var result = 0;
    if (!globalThis.bwWasmJitBrokerIsOom(
            new WebAssembly.CompileError('WebAssembly.Module(): out of memory'))) {
        result |= 1;
    }
    if (!globalThis.bwWasmJitBrokerIsOom(
            new Error('missing MT runtime group export b0'))) {
        result |= 2;
    }
    var table = new WebAssembly.Table({
        initial: 0,
        maximum: 0,
        element: 'anyfunc'
    });
    try {
        table.grow(1);
    } catch (error) {
        if (!globalThis.bwWasmJitBrokerIsOom(error)) {
            result |= 4;
        }
    }
    return result;
});

EM_JS(U32, testWasmJitMtRuntimeBatchStatsJs, (), {
    if (typeof Module.getWasmJitMtRuntimeBatchStats !== 'function' ||
            typeof Module._wasm_jit_mt_copy_runtime_batch_stats !== 'function') {
        return 0;
    }
    var expectedKeys = [
        'translatedFileBacked',
        'translatedAnonymous',
        'pendingEntries',
        'openBytes',
        'sealedGroups',
        'groupedModules',
        'groupedFunctions',
        'constructionAttempts',
        'constructionSuccesses',
        'maxFunctionsPerGroup',
        'countFlushes',
        'byteFlushes',
        'urgentFlushes',
        'processCapFlushes',
        'cancelledEntries',
        'permanentFailures',
        'groupedOomBlocks',
        'skippedConstructionAttempts',
        'groupInstanceCreations',
        'groupInstanceReuses',
        'groupedBrokerHits',
        'groupedLocalCompiles',
        'maxBlocks',
        'maxBatchBytes',
        'urgentPendingHits',
        'maxProcessOpenBytes',
        'averageFunctionsPerGroup'
    ];
    var result = 0;
    var nativeStats = Module.getWasmJitMtRuntimeBatchStats();
    if (nativeStats && Object.getPrototypeOf(nativeStats) === Object.prototype &&
            Object.keys(nativeStats).join('|') === expectedKeys.join('|') &&
            nativeStats.translatedFileBacked === 2 &&
            nativeStats.groupedModules === 1 &&
            nativeStats.groupedFunctions === 2 &&
            nativeStats.groupInstanceCreations === 1 &&
            nativeStats.groupInstanceReuses === 0 &&
            nativeStats.groupedBrokerHits === 0 &&
            nativeStats.groupedLocalCompiles === 1 &&
            nativeStats.averageFunctionsPerGroup === 2) {
        result |= 1;
    }

    var originalCopy = Module._wasm_jit_mt_copy_runtime_batch_stats;
    Module._wasm_jit_mt_copy_runtime_batch_stats = function(snapshot) {
        HEAPU32.fill(0, snapshot >>> 2, (snapshot + 192) >>> 2);
        HEAPU32[snapshot >>> 2] = 1;
        HEAPU32[(snapshot >>> 2) + 1] = 1;
        HEAPU32[(snapshot >>> 2) + 10] = 2;
        HEAPU32[(snapshot >>> 2) + 12] = 5;
    };
    try {
        var syntheticStats = Module.getWasmJitMtRuntimeBatchStats();
        if (syntheticStats.translatedFileBacked === 0x100000001 &&
                syntheticStats.groupedModules === 2 &&
                syntheticStats.groupedFunctions === 5 &&
                syntheticStats.averageFunctionsPerGroup === 2.5) {
            result |= 2;
        }
    } finally {
        Module._wasm_jit_mt_copy_runtime_batch_stats = originalCopy;
    }

    var originalMalloc = Module._malloc;
    var originalFree = Module._free;
    var allocated = 0;
    var freed = 0;
    Module._malloc = function(size) {
        allocated = originalMalloc(size);
        return allocated;
    };
    Module._free = function(address) {
        if (address === allocated) {
            freed += 1;
        }
        return originalFree(address);
    };
    Module._wasm_jit_mt_copy_runtime_batch_stats = function() {
        throw new Error('forced runtime batch snapshot copy failure');
    };
    try {
        Module.getWasmJitMtRuntimeBatchStats();
    } catch (error) {
        if (allocated && freed === 1 &&
                String(error).indexOf('forced runtime batch snapshot copy failure') >= 0) {
            result |= 4;
        }
    } finally {
        Module._wasm_jit_mt_copy_runtime_batch_stats = originalCopy;
        Module._malloc = originalMalloc;
        Module._free = originalFree;
    }
    var highPointer = 0x80000000;
    var runtimeStatsSource = String(Module.getWasmJitMtRuntimeBatchStats);
    if ((highPointer >>> 2) === 0x20000000 &&
            (highPointer >> 2) < 0 &&
            (runtimeStatsSource.indexOf('snapshot >>> 2') >= 0 ||
                runtimeStatsSource.indexOf('snapshot>>>2') >= 0)) {
        result |= 8;
    }
    return result;
});

struct WasmJitMtGroupedOomWorkerArgs {
    std::vector<U8> bytes;
    KMemory* memory = nullptr;
    U32 expectedIncarnation = 0;
    U32 actualIncarnation = 0;
    bool canConstructBefore = false;
    bool installed = false;
    bool canConstructAfter = false;
    WasmJitMtRuntimeGroupConstructorStatsSnapshot constructors;
    S32 completed = 0;
};

static void* testWasmJitMtGroupedOomOtherWorker(void* opaque) {
    WasmJitMtGroupedOomWorkerArgs* args = (WasmJitMtGroupedOomWorkerArgs*)opaque;
    args->actualIncarnation = wasmJitTestGetMtMemoryIncarnation(args->memory);
    args->canConstructBefore = wasmJitTestCanCurrentWorkerConstructFreshMtGroup(args->memory);
    wasmJitTestResetMtRuntimeGroupConstructorStats();
    std::vector<S32> slots;
    args->installed = wasmJitTestInstallMtRuntimeGroup(args->bytes, 2, args->memory, slots) &&
        slots.size() == 2 && slots[0] >= 0 && slots[1] >= 0;
    args->constructors = wasmJitTestGetMtRuntimeGroupConstructorStats();
    args->canConstructAfter = wasmJitTestCanCurrentWorkerConstructFreshMtGroup(args->memory);
    __atomic_store_n(&args->completed, 1, __ATOMIC_RELEASE);
    emscripten_futex_wake(&args->completed, INT_MAX);
    return nullptr;
}

static bool waitForWasmJitMtGroupedOomOtherWorker(WasmJitMtGroupedOomWorkerArgs* args) {
    double deadline = emscripten_get_now() + 5000.0;
    while (emscripten_get_now() < deadline) {
        S32 completed = __atomic_load_n(&args->completed, __ATOMIC_ACQUIRE);
        if (completed) {
            return true;
        }
        emscripten_futex_wait(&args->completed, completed, 50.0);
    }
    return false;
}

static bool runWasmJitMtGroupedOomOtherWorker(WasmJitMtGroupedOomWorkerArgs& args) {
    auto state = std::make_unique<WasmJitMtGroupedOomWorkerArgs>(args);
    WasmJitMtGroupedOomWorkerArgs* rawState = state.get();
    wasmJitTestPrepareMtThreadStart(rawState, rawState->memory);
    pthread_t thread;
    if (pthread_create(&thread, nullptr, testWasmJitMtGroupedOomOtherWorker, rawState)) {
        wasmJitTestCancelMtThreadStart(rawState);
        return false;
    }
    state.release();
    if (!waitForWasmJitMtGroupedOomOtherWorker(rawState)) {
        wasmJitTestCancelMtThreadStart(rawState);
        // A delayed worker can still start, so keep all heap-owned inputs alive.
        pthread_detach(thread);
        return false;
    }
    if (pthread_join(thread, nullptr)) {
        // Completion precedes the worker epilogue, so retain state unless joined.
        pthread_detach(thread);
        return false;
    }
    args = *rawState;
    delete rawState;
    return args.completed != 0;
}

struct WasmJitMtGroupWorkerStatsRaceArgs {
    S32 completed = 0;
};

struct WasmJitMtGroupWorkerStatsSnapshotArgs {
    WasmJitMtRuntimeBatchStatsSnapshot snapshot = {};
    S32 started = 0;
    S32 completed = 0;
};

static void* testWasmJitMtRecordGroupWorkerEvent(void* opaque) {
    WasmJitMtGroupWorkerStatsRaceArgs* args =
        (WasmJitMtGroupWorkerStatsRaceArgs*)opaque;
    wasm_jit_mt_record_group_worker_event(1);
    __atomic_store_n(&args->completed, 1, __ATOMIC_RELEASE);
    emscripten_futex_wake(&args->completed, INT_MAX);
    return nullptr;
}

static void* testWasmJitMtCopyGroupWorkerStats(void* opaque) {
    WasmJitMtGroupWorkerStatsSnapshotArgs* args =
        (WasmJitMtGroupWorkerStatsSnapshotArgs*)opaque;
    __atomic_store_n(&args->started, 1, __ATOMIC_RELEASE);
    emscripten_futex_wake(&args->started, INT_MAX);
    wasm_jit_mt_copy_runtime_batch_stats(&args->snapshot);
    __atomic_store_n(&args->completed, 1, __ATOMIC_RELEASE);
    emscripten_futex_wake(&args->completed, INT_MAX);
    return nullptr;
}

static bool waitForWasmJitMtGroupWorkerStatsValue(S32* value, double timeout) {
    double deadline = emscripten_get_now() + timeout;
    while (emscripten_get_now() < deadline) {
        S32 current = __atomic_load_n(value, __ATOMIC_ACQUIRE);
        if (current) {
            return true;
        }
        emscripten_futex_wait(value, current, 25.0);
    }
    return false;
}

static bool testWasmJitMtGroupWorkerStatsSnapshotCoherent(KMemory* memory) {
    WasmJitMtGroupWorkerStatsRaceArgs eventArgs;
    WasmJitMtGroupWorkerStatsSnapshotArgs snapshotArgs;
    wasmJitTestPauseNextMtGroupWorkerEvent();
    wasmJitTestPrepareMtThreadStart(&eventArgs, memory);
    pthread_t eventThread;
    if (pthread_create(&eventThread, nullptr,
            testWasmJitMtRecordGroupWorkerEvent, &eventArgs)) {
        wasmJitTestCancelMtThreadStart(&eventArgs);
        wasmJitTestReleaseMtGroupWorkerEvent();
        return false;
    }
    bool paused = wasmJitTestWaitForMtGroupWorkerEventPause();
    if (paused) {
        wasmJitTestPrepareMtThreadStart(&snapshotArgs, memory);
    }
    pthread_t snapshotThread;
    bool snapshotCreated = paused && !pthread_create(
        &snapshotThread, nullptr, testWasmJitMtCopyGroupWorkerStats, &snapshotArgs);
    if (paused && !snapshotCreated) {
        wasmJitTestCancelMtThreadStart(&snapshotArgs);
    }
    bool snapshotStarted = snapshotCreated &&
        waitForWasmJitMtGroupWorkerStatsValue(&snapshotArgs.started, 2000.0);
    if (snapshotStarted) {
        waitForWasmJitMtGroupWorkerStatsValue(&snapshotArgs.completed, 250.0);
    }
    wasmJitTestReleaseMtGroupWorkerEvent();
    bool eventJoined = pthread_join(eventThread, nullptr) == 0;
    bool snapshotJoined = snapshotCreated &&
        pthread_join(snapshotThread, nullptr) == 0;
    return paused && snapshotStarted && eventJoined && snapshotJoined &&
        eventArgs.completed && snapshotArgs.completed &&
        snapshotArgs.snapshot.groupInstanceCreations ==
            snapshotArgs.snapshot.groupedLocalCompiles +
                snapshotArgs.snapshot.groupedBrokerHits;
}

void testWasmJitMtRuntimeGrouping() {
    S32 oldEnabled = wasmJitTestSetMtModuleBrokerEnabled(1);
    struct RestoreMtRuntimeGroupTest {
        S32 oldEnabled;
        ~RestoreMtRuntimeGroupTest() {
            wasmJitTestFailMtRuntimeGroupAfterSlots(-1);
            wasmJitTestResetMtRuntimeBatching();
            wasmJitTestSetMtModuleBrokerEnabled(oldEnabled);
        }
    } restore{oldEnabled};

    testNewInstruction(0);
    wasmJitTestResetMtRuntimeBatching();
    if (!wasmJitTestAddMtSealedRequestForStats(testContext().memory, 76, 2) ||
            !wasmJitTestAddMtSealedRequestForStats(testContext().memory, 76, 3)) {
        testFail("MT WASM runtime batch creates two synthetic sealed requests");
        return;
    }
    WasmJitMtRuntimeBatchStatsSnapshot sealedStats = {};
    wasm_jit_mt_copy_runtime_batch_stats(&sealedStats);
    if (sealedStats.pendingEntries != 5 || sealedStats.openBytes != 0 ||
            sealedStats.sealedGroups != 2) {
        testFail("MT WASM runtime batch counts distinct same-key sealed requests");
        return;
    }
    wasmJitTestResetMtRuntimeBatching();
    WasmJitBatchLimits limits;
    limits.maxBlocks = 2;
    limits.maxBatchBytes = 1024 * 1024;
    limits.urgentPendingHits = 8;
    limits.maxProcessOpenBytes = 4 * 1024 * 1024;
    wasmJitTestSetMtBatchLimits(limits);
    wasmJitTestSetMtMappedFileKeyOverride(77);
    wasmJitTestEnableMtRuntimeBatching(true);

    U32 addresses[2];
    for (U32 i = 0; i < 2; ++i) {
        addresses[i] = testContext().codeIp;
        testPushCode8(i == 0 ? 0x40 : 0x41); // inc eax / inc ecx
        testPushCode8(0xcd);
        testPushCode8(0x97);
    }
    auto compile = [&](U32 address) {
        DecodedOp* op = testContext().cpu->getOp(address, 0);
        startNewJIT(testContext().cpu, address, op);
        return op;
    };
    U32 constructionCount = wasmJitTestMtRuntimeGroupConstructionCount();
    DecodedOp* first = compile(addresses[0]);
    if (!(first->flags2 & OP_FLAG2_WASM_JIT_PENDING) || first->pfnJitCode ||
            wasmJitTestMtPendingCount() != 1 || wasmJitTestMtRuntimeModuleCount() != 0 ||
            wasmJitTestMtRuntimeGroupConstructionCount() != constructionCount) {
        WasmJitMtRuntimeStatsSnapshot failedStats = wasmJitTestGetMtRuntimeStats();
        WasmJitMtBrokerModuleRef failedRef = wasmJitTestGetMtBrokerSlotRef(
            (int)(uintptr_t)first->pfnJitCode);
        testFail("MT WASM runtime batch first mapped block remains pending: pendingFlag=%u jit=%u pending=%u modules=%u constructions=%u/%u translated=%llu/%llu moduleId=%u owner=%u/%u",
            (first->flags2 & OP_FLAG2_WASM_JIT_PENDING) != 0,
            first->pfnJitCode != nullptr, wasmJitTestMtPendingCount(),
            wasmJitTestMtRuntimeModuleCount(),
            wasmJitTestMtRuntimeGroupConstructionCount(), constructionCount,
            failedStats.translatedFileBacked, failedStats.translatedAnonymous,
            failedRef.moduleId, failedRef.memoryId, failedRef.memoryIncarnation);
        return;
    }

    DecodedOp* second = compile(addresses[1]);
    if (!first->pfnJitCode || !second->pfnJitCode || first->pfnJitCode == second->pfnJitCode ||
            (first->flags2 & OP_FLAG2_WASM_JIT_PENDING) || (second->flags2 & OP_FLAG2_WASM_JIT_PENDING)) {
        testFail("MT WASM runtime batch publishes distinct callable slots");
        return;
    }
    WasmJitMtRuntimeStatsSnapshot stats = wasmJitTestGetMtRuntimeStats();
    if (wasmJitTestMtPendingCount() != 0 || wasmJitTestMtRuntimeGroupCount() != 1 ||
            wasmJitTestMtRuntimeModuleCount() != 1 ||
            wasmJitTestMtRuntimeGroupConstructionCount() != constructionCount + 1 ||
            stats.translatedFileBacked != 2 || stats.groupedModules != 1 ||
            stats.groupedFunctions != 2 || stats.countFlushes != 1 ||
            stats.constructionAttempts != 1 || stats.constructionSuccesses != 1 ||
            stats.maxFunctionsPerGroup != 2) {
        testFail("MT WASM runtime batch creates one two-function group");
        return;
    }
    WasmJitMtRuntimeBatchStatsSnapshot batchStats = {};
    wasm_jit_mt_copy_runtime_batch_stats(&batchStats);
    if (batchStats.translatedFileBacked != 2 || batchStats.groupedModules != 1 ||
            batchStats.groupedFunctions != 2 || batchStats.constructionAttempts != 1 ||
            batchStats.constructionSuccesses != 1 || batchStats.maxFunctionsPerGroup != 2 ||
            batchStats.countFlushes != 1 || batchStats.pendingEntries != 0 ||
            batchStats.openBytes != 0 || batchStats.sealedGroups != 0 ||
            batchStats.groupInstanceCreations != 1 || batchStats.groupInstanceReuses != 0 ||
            batchStats.groupedBrokerHits != 0 || batchStats.groupedLocalCompiles != 1) {
        testFail("MT WASM runtime batch exported snapshot matches grouped publication");
        return;
    }
    if (testWasmJitMtRuntimeBatchStatsJs() != 15) {
        testFail("MT WASM runtime batch JavaScript diagnostics shape and U64 decoding");
        return;
    }
    if (!wasmJitTestBrokerStatsSnapshotFailureCompletes()) {
        testFail("MT WASM broker stats rejects failed asynchronous runtime snapshot");
        return;
    }
    if (!wasmJitTestBrokerIncompleteStats(1, 0, 0, 1)) {
        testFail("MT WASM broker stats retain exact grouped totals with missing workers");
        return;
    }
    if (!testWasmJitMtGroupWorkerStatsSnapshotCoherent(testContext().memory)) {
        testFail("MT WASM broker grouped totals use one coherent event boundary");
        return;
    }

    testContext().cpu->reg[0].u32 = 0;
    wasmStartJITOp(testContext().cpu, first);
    if (testContext().cpu->reg[0].u32 != 1) {
        testFail("MT WASM runtime batch first block executes");
    }
    testContext().cpu->reg[1].u32 = 0;
    wasmStartJITOp(testContext().cpu, second);
    if (testContext().cpu->reg[1].u32 != 1) {
        testFail("MT WASM runtime batch second block executes");
    }

    wasmJitTestResetMtRuntimeBatching();
    testNewInstruction(0);
    wasmJitTestSetMtBatchLimits(limits);
    wasmJitTestEnableMtRuntimeBatching(true);
    for (U32 i = 0; i < 2; ++i) {
        addresses[i] = testContext().codeIp;
        testPushCode8(0x40 + i);
        testPushCode8(0xcd);
        testPushCode8(0x97);
    }
    wasmJitTestSetMtMappedFileKeyOverride(77);
    first = compile(addresses[0]);
    wasmJitTestSetMtMappedFileKeyOverride(78);
    second = compile(addresses[1]);
    if (!(first->flags2 & OP_FLAG2_WASM_JIT_PENDING) || !(second->flags2 & OP_FLAG2_WASM_JIT_PENDING) ||
            wasmJitTestMtPendingCount() != 2 || wasmJitTestMtPendingRawBytes() == 0 ||
            wasmJitTestMtRuntimeModuleCount() != 0) {
        testFail("MT WASM runtime batches isolate mapped-file keys");
    }

    wasmJitTestResetMtRuntimeBatching();
    testNewInstruction(0);
    wasmJitTestSetMtBatchLimits(limits);
    wasmJitTestSetMtMappedFileKeyOverride(0);
    wasmJitTestEnableMtRuntimeBatching(true);
    U32 anonymousAddress = testContext().codeIp;
    testPushCode8(0x40);
    testPushCode8(0x41);
    testPushCode8(0x42);
    testPushCode8(0x43);
    testPushCode8(0xcd);
    testPushCode8(0x97);
    DecodedOp* anonymous = compile(anonymousAddress);
    stats = wasmJitTestGetMtRuntimeStats();
    if (!anonymous->pfnJitCode || (anonymous->flags2 & OP_FLAG2_WASM_JIT_PENDING) ||
            wasmJitTestMtPendingCount() != 0 || wasmJitTestMtRuntimeGroupCount() != 0 ||
            stats.translatedAnonymous != 1 || stats.standaloneModules != 1) {
        testFail("MT WASM fresh anonymous four-op block stays standalone");
    }

    std::vector<U8> directFirst = makeMergeInput(0x11223344);
    std::vector<U8> directSecond = makeMergeInput(0x55667788);
    std::vector<WasmJitMergeInput> inputs = {{&directFirst}, {&directSecond}};
    std::vector<U8> merged;
    BString error;
    if (!wasmJitMergeModules(inputs, merged, error)) {
        testFail("MT WASM runtime group rollback merge setup: %s", error.c_str());
        return;
    }
    S32 failedFirstSlot = (S32)(uintptr_t)anonymous->pfnJitCode + 1;
    wasmJitTestFailMtRuntimeGroupAfterSlots(1);
    std::vector<S32> failedSlots;
    if (wasmJitTestInstallMtRuntimeGroup(merged, 2, testContext().memory, failedSlots)) {
        testFail("MT WASM runtime group reports partial install failure");
    }
    if (failedSlots.size() != 2 || failedSlots[0] != -1 || failedSlots[1] != -1) {
        testFail("MT WASM runtime group resets failed output slots");
    }
    if (wasmJitTestLazyInstallMtSlot(failedFirstSlot)) {
        testFail("MT WASM runtime group does not publish failed slot metadata");
    }
    if (!testWasmJitMtRuntimeGroupSlotIsClear(failedFirstSlot)) {
        testFail("MT WASM runtime group clears partial table installation");
    }
    wasmJitTestFailMtRuntimeGroupAfterSlots(-1);
    wasmJitTestResetMtRuntimeBatching();
    batchStats = {};
    wasm_jit_mt_copy_runtime_batch_stats(&batchStats);
    if (batchStats.maxBlocks != 64 || batchStats.maxBatchBytes != 512 * 1024 ||
            batchStats.urgentPendingHits != 64 ||
            batchStats.maxProcessOpenBytes != 4 * 1024 * 1024) {
        testFail("MT WASM runtime batch exported snapshot restores production limits");
    }
}

void testWasmJitMtGroupedOomBlock() {
    S32 oldEnabled = wasmJitTestSetMtModuleBrokerEnabled(1);
    struct RestoreMtGroupedOomTest {
        S32 oldEnabled;
        KMemory* memory;

        ~RestoreMtGroupedOomTest() {
            wasmJitTestSetMtBrokerCompileOom(0, false);
            wasmJitTestForceNextMtRuntimeGroupInstanceOom(0);
            wasmJitTestFailMtRuntimeGroupAfterSlots(-1);
            wasmJitTestResetMtRuntimeGroupConstructorStats();
            wasmJitTestResetMtRuntimeBatching();
            wasmJitTestSetMtModuleBrokerEnabled(1);
            wasmJitTestInvalidateMtBrokerMemory(memory);
            wasmJitTestSetMtModuleBrokerEnabled(oldEnabled);
        }
    } restore{oldEnabled, testContext().memory};

    if (testWasmJitMtOomClassifierExclusions() != 7) {
        testFail("MT WASM grouped OOM classifier excludes non-constructor failures");
    }

    testNewInstruction(0);
    wasmJitTestResetMtRuntimeBatching();
    WasmJitBatchLimits limits;
    limits.maxBlocks = 2;
    limits.maxBatchBytes = 1024 * 1024;
    limits.urgentPendingHits = 8;
    limits.maxProcessOpenBytes = 4 * 1024 * 1024;
    wasmJitTestSetMtBatchLimits(limits);
    wasmJitTestEnableMtRuntimeBatching(true);

    std::vector<U8> reusableFirst = makeMergeInput(0x11223344);
    std::vector<U8> reusableSecond = makeMergeInput(0x55667788);
    std::vector<WasmJitMergeInput> reusableInputs = {{&reusableFirst}, {&reusableSecond}};
    std::vector<U8> reusableGroup;
    BString mergeError;
    if (!wasmJitMergeModules(reusableInputs, reusableGroup, mergeError)) {
        testFail("MT WASM grouped OOM reusable group merge setup: %s", mergeError.c_str());
        return;
    }

    std::vector<S32> reusableSlots;
    if (!wasmJitTestInstallMtRuntimeGroup(
            reusableGroup, 2, testContext().memory, reusableSlots) ||
            reusableSlots.size() != 2 || reusableSlots[0] < 0 || reusableSlots[1] < 0) {
        testFail("MT WASM grouped OOM publishes a reusable grouped module");
        return;
    }
    WasmJitMtBrokerModuleRef reusableRef =
        wasmJitTestGetMtBrokerSlotRef(reusableSlots[0]);
    if (!reusableRef.moduleId ||
            !wasmJitTestDropCurrentWorkerMtRuntimeGroupInstance(reusableSlots[0]) ||
            !wasmJitTestClearCurrentWorkerMtSlot(reusableSlots[0])) {
        testFail("MT WASM grouped OOM prepares a lazy broker-cache install");
        return;
    }

    auto addBlock = [&](U32 registerIndex, U32 opCount) {
        U32 address = testContext().codeIp;
        for (U32 i = 0; i < opCount; ++i) {
            testPushCode8(0x40 + ((registerIndex + i) & 3));
        }
        testPushCode8(0xcd);
        testPushCode8(0x97);
        return address;
    };
    auto addPair = [&](U32 registerIndex) {
        std::array<U32, 2> result;
        result[0] = addBlock(registerIndex, 1);
        result[1] = addBlock(registerIndex + 1, 1);
        return result;
    };
    auto compile = [&](U32 address) {
        DecodedOp* op = testContext().cpu->getOp(address, 0);
        startNewJIT(testContext().cpu, address, op);
        return op;
    };
    auto compilePair = [&](const std::array<U32, 2>& addresses, S32 mappedFileKey) {
        wasmJitTestSetMtMappedFileKeyOverride(mappedFileKey);
        std::array<DecodedOp*, 2> result;
        result[0] = compile(addresses[0]);
        result[1] = compile(addresses[1]);
        return result;
    };
    auto isInterpreted = [&](DecodedOp* op) {
        return op && !op->pfnJitCode && !(op->flags & OP_FLAG_JIT) &&
            !(op->flags2 & OP_FLAG2_WASM_JIT_PENDING) && op->pfn;
    };

    std::array<U32, 2> compileOomAddresses = addPair(0);
    std::array<U32, 2> blockedAddresses = addPair(2);
    U32 anonymousAddress = addBlock(0, 4);
    std::array<U32, 2> permanentFailureAddresses = addPair(0);
    std::array<U32, 2> afterPermanentFailureAddresses = addPair(2);
    std::array<U32, 2> instanceOomAddresses = addPair(0);
    std::array<U32, 2> instanceBlockedAddresses = addPair(2);

    U32 blockedMemoryId = (U32)(uintptr_t)testContext().memory;
    U32 blockedIncarnation = wasmJitTestGetMtMemoryIncarnation(testContext().memory);
    U32 syntheticIncarnation = blockedIncarnation ^ 0x80000000u;
    if (!syntheticIncarnation || syntheticIncarnation == blockedIncarnation) {
        syntheticIncarnation = blockedIncarnation == 1 ? 2 : 1;
    }
    wasmJitTestResetMtRuntimeGroupConstructorStats();
    wasmJitTestSetMtModuleBrokerEnabled(0);
    wasmJitTestSetMtBrokerCompileOom(0, true);
    std::array<DecodedOp*, 2> compileOomOps = compilePair(compileOomAddresses, 201);
    wasmJitTestSetMtBrokerCompileOom(0, false);
    wasmJitTestSetMtModuleBrokerEnabled(1);

    WasmJitMtRuntimeStatsSnapshot stats = wasmJitTestGetMtRuntimeStats();
    WasmJitMtRuntimeGroupConstructorStatsSnapshot constructors =
        wasmJitTestGetMtRuntimeGroupConstructorStats();
    if (!isInterpreted(compileOomOps[0]) || !isInterpreted(compileOomOps[1]) ||
            wasmJitTestMtPendingCount() != 0 || wasmJitTestMtOpenCount() != 0 ||
            wasmJitTestMtSealedCount() != 0 ||
            stats.constructionAttempts != 1 || stats.groupedOomBlocks != 1 ||
            stats.skippedConstructionAttempts != 0 || stats.permanentFailures != 0 ||
            constructors.moduleAttempts != 1 || constructors.instanceAttempts != 0 ||
            !wasmJitTestIsCurrentWorkerFreshMtGroupBlocked(
                blockedMemoryId, blockedIncarnation) ||
            wasmJitTestIsCurrentWorkerFreshMtGroupBlocked(
                blockedMemoryId, syntheticIncarnation) ||
            wasmJitTestCanCurrentWorkerConstructFreshMtGroup(testContext().memory)) {
        testFail("MT WASM grouped compile OOM blocks one exact Worker owner incarnation");
    }

    std::array<DecodedOp*, 2> blockedOps = compilePair(blockedAddresses, 202);
    stats = wasmJitTestGetMtRuntimeStats();
    constructors = wasmJitTestGetMtRuntimeGroupConstructorStats();
    if (!isInterpreted(blockedOps[0]) || !isInterpreted(blockedOps[1]) ||
            wasmJitTestMtPendingCount() != 0 || wasmJitTestMtOpenCount() != 0 ||
            wasmJitTestMtSealedCount() != 0 ||
            stats.constructionAttempts != 1 || stats.groupedOomBlocks != 1 ||
            stats.skippedConstructionAttempts != 1 || stats.permanentFailures != 0 ||
            constructors.moduleAttempts != 1 || constructors.instanceAttempts != 0) {
        testFail("MT WASM grouped OOM skips the next fresh group without constructors");
    }

    WasmJitMtBrokerStatsSnapshot beforeCacheHit =
        wasmJitTestGetMtBrokerStats(testContext().memory);
    S32 cacheValue = 0;
    bool cacheInstalled = wasmJitTestLazyInstallMtSlot(reusableSlots[0]);
    bool cacheExecuted = cacheInstalled &&
        testWasmJitMtCallRuntimeGroupSlot(reusableSlots[0], &cacheValue) != 0;
    WasmJitMtBrokerStatsSnapshot afterCacheHit =
        wasmJitTestGetMtBrokerStats(testContext().memory);
    constructors = wasmJitTestGetMtRuntimeGroupConstructorStats();
    if (!cacheExecuted || cacheValue != 0x11223344 ||
            afterCacheHit.brokerHits != beforeCacheHit.brokerHits + 1 ||
            constructors.moduleAttempts != 1 || constructors.instanceAttempts != 1) {
        testFail("MT WASM grouped OOM preserves lazy grouped broker-cache hits");
    }

    WasmJitMtGroupedOomWorkerArgs workerArgs;
    workerArgs.bytes = reusableGroup;
    workerArgs.memory = testContext().memory;
    workerArgs.expectedIncarnation = blockedIncarnation;
    if (!runWasmJitMtGroupedOomOtherWorker(workerArgs) ||
            workerArgs.actualIncarnation != workerArgs.expectedIncarnation ||
            !workerArgs.canConstructBefore || !workerArgs.installed ||
            !workerArgs.canConstructAfter ||
            workerArgs.constructors.moduleAttempts != 1 ||
            workerArgs.constructors.instanceAttempts != 1 ||
            wasmJitTestCanCurrentWorkerConstructFreshMtGroup(testContext().memory)) {
        testFail("MT WASM grouped OOM block stays on one physical Worker");
    }

    wasmJitTestSetMtMappedFileKeyOverride(0);
    DecodedOp* anonymous = compile(anonymousAddress);
    stats = wasmJitTestGetMtRuntimeStats();
    if (!anonymous->pfnJitCode || (anonymous->flags2 & OP_FLAG2_WASM_JIT_PENDING) ||
            stats.translatedAnonymous != 1 || stats.standaloneModules != 1) {
        testFail("MT WASM grouped OOM leaves fresh anonymous standalone JIT usable");
    }

    wasmJitTestInvalidateMtBrokerMemory(testContext().memory);
    U32 replacementIncarnation = wasmJitTestGetMtMemoryIncarnation(testContext().memory);
    if (!blockedIncarnation || !replacementIncarnation ||
            replacementIncarnation == blockedIncarnation ||
            wasmJitTestIsCurrentWorkerFreshMtGroupBlocked(
                blockedMemoryId, blockedIncarnation) ||
            wasmJitTestIsCurrentWorkerFreshMtGroupBlocked(
                blockedMemoryId, replacementIncarnation) ||
            !wasmJitTestCanCurrentWorkerConstructFreshMtGroup(testContext().memory)) {
        testFail("MT WASM grouped OOM exact purge removes only the retired incarnation block");
    }

    wasmJitTestResetMtRuntimeBatching();
    wasmJitTestSetMtBatchLimits(limits);
    wasmJitTestEnableMtRuntimeBatching(true);
    wasmJitTestResetMtRuntimeGroupConstructorStats();
    wasmJitTestFailMtRuntimeGroupAfterSlots(1);
    std::array<DecodedOp*, 2> permanentFailureOps =
        compilePair(permanentFailureAddresses, 210);
    wasmJitTestFailMtRuntimeGroupAfterSlots(-1);
    stats = wasmJitTestGetMtRuntimeStats();
    if (!isInterpreted(permanentFailureOps[0]) || !isInterpreted(permanentFailureOps[1]) ||
            stats.permanentFailures == 0 || stats.groupedOomBlocks != 0 ||
            stats.skippedConstructionAttempts != 0 ||
            !wasmJitTestCanCurrentWorkerConstructFreshMtGroup(testContext().memory)) {
        testFail("MT WASM grouped permanent failure does not classify or block as OOM");
    }

    std::array<DecodedOp*, 2> afterPermanentFailureOps =
        compilePair(afterPermanentFailureAddresses, 211);
    stats = wasmJitTestGetMtRuntimeStats();
    if (!afterPermanentFailureOps[0]->pfnJitCode ||
            !afterPermanentFailureOps[1]->pfnJitCode ||
            stats.constructionAttempts != 2 || stats.constructionSuccesses != 1 ||
            stats.groupedModules != 1 || stats.groupedOomBlocks != 0 ||
            stats.skippedConstructionAttempts != 0) {
        testFail("MT WASM grouped permanent failure permits a later unrelated group");
    }

    wasmJitTestResetMtRuntimeBatching();
    wasmJitTestSetMtBatchLimits(limits);
    wasmJitTestEnableMtRuntimeBatching(true);
    wasmJitTestResetMtRuntimeGroupConstructorStats();
    wasmJitTestForceNextMtRuntimeGroupInstanceOom(1);
    std::array<DecodedOp*, 2> instanceOomOps = compilePair(instanceOomAddresses, 220);
    stats = wasmJitTestGetMtRuntimeStats();
    constructors = wasmJitTestGetMtRuntimeGroupConstructorStats();
    if (!isInterpreted(instanceOomOps[0]) || !isInterpreted(instanceOomOps[1]) ||
            wasmJitTestMtPendingCount() != 0 || wasmJitTestMtOpenCount() != 0 ||
            wasmJitTestMtSealedCount() != 0 ||
            stats.constructionAttempts != 1 || stats.groupedOomBlocks != 1 ||
            stats.skippedConstructionAttempts != 0 || stats.permanentFailures != 0 ||
            constructors.moduleAttempts != 1 || constructors.instanceAttempts != 1 ||
            wasmJitTestCanCurrentWorkerConstructFreshMtGroup(testContext().memory)) {
        testFail("MT WASM grouped instance OOM blocks after module acquisition");
    }

    std::array<DecodedOp*, 2> instanceBlockedOps =
        compilePair(instanceBlockedAddresses, 221);
    stats = wasmJitTestGetMtRuntimeStats();
    constructors = wasmJitTestGetMtRuntimeGroupConstructorStats();
    if (!isInterpreted(instanceBlockedOps[0]) || !isInterpreted(instanceBlockedOps[1]) ||
            wasmJitTestMtPendingCount() != 0 || wasmJitTestMtOpenCount() != 0 ||
            wasmJitTestMtSealedCount() != 0 ||
            stats.constructionAttempts != 1 || stats.groupedOomBlocks != 1 ||
            stats.skippedConstructionAttempts != 1 || stats.permanentFailures != 0 ||
            constructors.moduleAttempts != 1 || constructors.instanceAttempts != 1) {
        testFail("MT WASM grouped instance OOM blocks another fresh constructor attempt");
    }
    WasmJitMtRuntimeBatchStatsSnapshot batchStats = {};
    wasm_jit_mt_copy_runtime_batch_stats(&batchStats);
    if (batchStats.groupedOomBlocks != 1 ||
            batchStats.skippedConstructionAttempts != 1) {
        testFail("MT WASM grouped OOM exported snapshot matches final blocked sequence");
    }
}

void testWasmJitMtPendingLifecycle() {
    S32 oldEnabled = wasmJitTestSetMtModuleBrokerEnabled(1);
    struct RestoreMtPendingTest {
        S32 oldEnabled;
        ~RestoreMtPendingTest() {
            wasmJitTestCancelMtSealedEntryBeforePublish(-1);
            wasmJitTestReplaceMtMemoryBeforePublish(0, nullptr);
            wasmJitTestSetMtPendingByteChargeOverride(0);
            wasmJitTestResetMtRuntimeBatching();
            wasmJitTestSetMtModuleBrokerEnabled(oldEnabled);
        }
    } restore{oldEnabled};

    WasmJitBatchLimits productionLimits;
    auto beginPhase = [&](const WasmJitBatchLimits& limits, S32 mappedFileKey) {
        wasmJitTestCancelMtSealedEntryBeforePublish(-1);
        wasmJitTestReplaceMtMemoryBeforePublish(0, nullptr);
        wasmJitTestSetMtPendingByteChargeOverride(0);
        wasmJitTestResetMtRuntimeBatching();
        testNewInstruction(0);
        wasmJitTestSetMtBatchLimits(limits);
        wasmJitTestSetMtMappedFileKeyOverride(mappedFileKey);
        wasmJitTestEnableMtRuntimeBatching(true);
    };
    auto addBlocks = [&](U32 count) {
        std::vector<U32> addresses;
        addresses.reserve(count);
        for (U32 i = 0; i < count; ++i) {
            addresses.push_back(testContext().codeIp);
            testPushCode8(0x40 + (i & 3));
            testPushCode8(0xcd);
            testPushCode8(0x97);
        }
        return addresses;
    };
    auto compile = [&](CPU* cpu, U32 address) {
        DecodedOp* op = cpu->getOp(address, 0);
        startNewJIT(cpu, address, op);
        return op;
    };
    auto runPending = [&](CPU* cpu, U32 address, DecodedOp* op) {
        cpu->eip.u32 = address - cpu->seg[CS].address;
        cpu->nextOp = op;
        cpu->run();
    };
    auto restoreMappings = [&]() {
        constexpr U32 pagesPerSegment = 32;
        U32 segmentBytes = pagesPerSegment << K_PAGE_SHIFT;
        U32 stackBase = TEST_STACK_ADDRESS - segmentBytes;
        if (testContext().memory->mmap(testContext().thread, stackBase, segmentBytes,
                    K_PROT_WRITE | K_PROT_READ, K_MAP_FIXED | K_MAP_PRIVATE, -1, 0) != stackBase ||
                testContext().memory->mmap(testContext().thread, TEST_CODE_ADDRESS, segmentBytes,
                    K_PROT_WRITE | K_PROT_READ | K_PROT_EXEC, K_MAP_FIXED | K_MAP_PRIVATE, -1, 0) != TEST_CODE_ADDRESS ||
                testContext().memory->mmap(testContext().thread, TEST_HEAP_ADDRESS, segmentBytes,
                    K_PROT_READ | K_PROT_WRITE, K_MAP_FIXED | K_MAP_PRIVATE, -1, 0) != TEST_HEAP_ADDRESS) {
            testFail("MT WASM pending lifecycle restores mappings after memory reset");
        }
    };

    beginPhase(productionLimits, 101);
    std::vector<U32> addresses = addBlocks(64);
    std::vector<DecodedOp*> countOps;
    countOps.reserve(addresses.size());
    for (U32 address : addresses) {
        countOps.push_back(compile(testContext().cpu, address));
    }
    WasmJitMtRuntimeStatsSnapshot stats = wasmJitTestGetMtRuntimeStats();
    if (wasmJitTestMtPendingCount() != 0 || wasmJitTestMtRuntimeModuleCount() != 1 ||
            stats.countFlushes != 1 || stats.groupedFunctions != 64 ||
            !countOps.front()->pfnJitCode || !countOps.back()->pfnJitCode) {
        testFail("MT WASM pending lifecycle flushes the 64th same-mapping block");
    }

    beginPhase(productionLimits, 102);
    addresses = addBlocks(2);
    wasmJitTestSetMtPendingByteChargeOverride(300 * 1024);
    DecodedOp* byteFirst = compile(testContext().cpu, addresses[0]);
    DecodedOp* byteSecond = compile(testContext().cpu, addresses[1]);
    wasmJitTestSetMtPendingByteChargeOverride(0);
    stats = wasmJitTestGetMtRuntimeStats();
    if (!byteFirst->pfnJitCode || !(byteSecond->flags2 & OP_FLAG2_WASM_JIT_PENDING) ||
            wasmJitTestMtPendingCount() != 1 || stats.byteFlushes != 1 ||
            stats.groupedModules != 1 || stats.groupedFunctions != 1) {
        testFail("MT WASM pending lifecycle flushes before a 512 KiB overflow entry");
    }

    WasmJitBatchLimits orderedFlushLimits = productionLimits;
    orderedFlushLimits.maxProcessOpenBytes = 450 * 1024;
    beginPhase(orderedFlushLimits, 110);
    addresses = addBlocks(3);
    wasmJitTestSetMtPendingByteChargeOverride(100 * 1024);
    wasmJitTestSetMtMappedFileKeyOverride(111);
    DecodedOp* oldest = compile(testContext().cpu, addresses[0]);
    wasmJitTestSetMtPendingByteChargeOverride(300 * 1024);
    wasmJitTestSetMtMappedFileKeyOverride(110);
    DecodedOp* byteBatch = compile(testContext().cpu, addresses[1]);
    wasmJitTestSetMtPendingByteChargeOverride(400 * 1024);
    DecodedOp* incoming = compile(testContext().cpu, addresses[2]);
    wasmJitTestSetMtPendingByteChargeOverride(0);
    stats = wasmJitTestGetMtRuntimeStats();
    if (!oldest->pfnJitCode || !byteBatch->pfnJitCode ||
            !(incoming->flags2 & OP_FLAG2_WASM_JIT_PENDING) ||
            wasmJitTestMtPendingCount() != 1 || stats.byteFlushes != 1 ||
            stats.processCapFlushes != 1 || stats.groupedModules != 2) {
        testFail("MT WASM pending lifecycle processes ordered byte and process flush requests");
    }

    WasmJitBatchLimits processLimits = productionLimits;
    processLimits.maxBatchBytes = 8 * 1024 * 1024;
    beginPhase(processLimits, 120);
    addresses = addBlocks(2);
    wasmJitTestSetMtPendingByteChargeOverride(2 * 1024 * 1024 + 1);
    DecodedOp* processOldest = compile(testContext().cpu, addresses[0]);
    wasmJitTestSetMtMappedFileKeyOverride(121);
    DecodedOp* processNewest = compile(testContext().cpu, addresses[1]);
    wasmJitTestSetMtPendingByteChargeOverride(0);
    stats = wasmJitTestGetMtRuntimeStats();
    if (!processOldest->pfnJitCode || !(processNewest->flags2 & OP_FLAG2_WASM_JIT_PENDING) ||
            wasmJitTestMtPendingCount() != 1 || stats.processCapFlushes != 1 ||
            stats.byteFlushes != 0 || stats.groupedFunctions != 1) {
        testFail("MT WASM pending lifecycle flushes the oldest mapping above the 4 MiB process cap");
    }

    beginPhase(productionLimits, 122);
    addresses = addBlocks(9);
    wasmJitTestSetMtPendingByteChargeOverride(511 * 1024);
    std::vector<DecodedOp*> productionCapOps;
    productionCapOps.reserve(addresses.size());
    for (U32 i = 0; i < addresses.size(); ++i) {
        wasmJitTestSetMtMappedFileKeyOverride(122 + i);
        productionCapOps.push_back(compile(testContext().cpu, addresses[i]));
    }
    wasmJitTestSetMtPendingByteChargeOverride(0);
    stats = wasmJitTestGetMtRuntimeStats();
    if (!productionCapOps.front()->pfnJitCode ||
            !(productionCapOps.back()->flags2 & OP_FLAG2_WASM_JIT_PENDING) ||
            wasmJitTestMtPendingCount() != 8 || stats.processCapFlushes != 1 ||
            stats.byteFlushes != 0 || stats.groupedFunctions != 1) {
        testFail("MT WASM pending lifecycle enforces the 4 MiB cap with production batch limits");
    }

    beginPhase(productionLimits, 130);
    addresses = addBlocks(1);
    DecodedOp* urgent = compile(testContext().cpu, addresses[0]);
    U32 translations = wasmJitTestMtTranslationCount();
    startNewJIT(testContext().cpu, addresses[0], urgent);
    if (wasmJitTestMtTranslationCount() != translations || wasmJitTestMtPendingCount() != 1) {
        testFail("MT WASM pending lifecycle does not translate or enqueue a pending block twice");
    }
    urgent->runCount = JIT_RUN_COUNT_MAX; // catches: a pending hit wrapping the counter past its top
    for (U32 i = 0; i < 63; ++i) {
        runPending(testContext().cpu, addresses[0], urgent);
    }
    if (!(urgent->flags2 & OP_FLAG2_WASM_JIT_PENDING) || urgent->pfnJitCode ||
            urgent->runCount != JIT_RUN_COUNT_MAX || wasmJitTestMtTranslationCount() != translations ||
            wasmJitTestMtPendingCount() != 1) {
        testFail("MT WASM pending lifecycle saturates runCount without retranslating before hit 64");
    }
    runPending(testContext().cpu, addresses[0], urgent);
    stats = wasmJitTestGetMtRuntimeStats();
    if (!urgent->pfnJitCode || (urgent->flags2 & OP_FLAG2_WASM_JIT_PENDING) ||
            urgent->runCount != JIT_RUN_COUNT_MAX || wasmJitTestMtTranslationCount() != translations ||
            wasmJitTestMtPendingCount() != 0 || stats.urgentFlushes != 1 ||
            stats.groupedModules != 1 || stats.groupedFunctions != 1) {
        testFail("MT WASM pending lifecycle flushes one group on the 64th pending hit: jit=%d pendingFlag=%d runCount=%u translations=%u/%u pending=%u urgent=%llu groups=%llu functions=%llu",
            urgent->pfnJitCode != nullptr, (urgent->flags2 & OP_FLAG2_WASM_JIT_PENDING) != 0,
            urgent->runCount, wasmJitTestMtTranslationCount(), translations,
            wasmJitTestMtPendingCount(), stats.urgentFlushes, stats.groupedModules, stats.groupedFunctions);
    }

    beginPhase(productionLimits, 140);
    addresses = addBlocks(2);
    DecodedOp* generic = compile(testContext().cpu, addresses[0]);
    {
        std::unique_lock<std::recursive_mutex> lock(testContext().memory->mutex);
        jitCodeInvalidated(testContext().memory, {generic}, {});
    }
    if ((generic->flags2 & OP_FLAG2_WASM_JIT_PENDING) || wasmJitTestMtPendingCount() != 0) {
        testFail("MT WASM pending lifecycle generic invalidation cancels an open entry");
    }
    DecodedOp* removed = compile(testContext().cpu, addresses[1]);
    testContext().memory->removeCodeBlock(addresses[1], removed, false);
    if (wasmJitTestMtPendingCount() != 0) {
        testFail("MT WASM pending lifecycle removeCodeBlock cancels an open entry");
    }

    WasmJitBatchLimits pairLimits = productionLimits;
    pairLimits.maxBlocks = 2;
    beginPhase(pairLimits, 150);
    addresses = addBlocks(2);
    DecodedOp* cancelledSealed = compile(testContext().cpu, addresses[0]);
    wasmJitTestCancelMtSealedEntryBeforePublish(0);
    DecodedOp* publishedPeer = compile(testContext().cpu, addresses[1]);
    if (cancelledSealed->pfnJitCode || (cancelledSealed->flags2 & OP_FLAG2_WASM_JIT_PENDING) ||
            !publishedPeer->pfnJitCode || wasmJitTestMtPendingCount() != 0) {
        testFail("MT WASM pending lifecycle cancels a sealed entry before publication");
    }

    beginPhase(pairLimits, 151);
    addresses = addBlocks(2);
    U32 lateOldIncarnation = wasmJitTestGetMtMemoryIncarnation(testContext().memory);
    compile(testContext().cpu, addresses[0]);
    DecodedOp* replacement = nullptr;
    wasmJitTestReplaceMtMemoryBeforePublish(addresses[0], &replacement);
    compile(testContext().cpu, addresses[1]);
    U32 lateReplacementIncarnation = wasmJitTestGetMtMemoryIncarnation(testContext().memory);
    stats = wasmJitTestGetMtRuntimeStats();
    if (!replacement || testContext().cpu->getOp(addresses[0], 0) != replacement ||
            replacement->pfnJitCode || (replacement->flags & OP_FLAG_JIT) ||
            (replacement->flags2 & OP_FLAG2_WASM_JIT_PENDING) ||
            wasmJitTestMtPendingCount() != 0 ||
            !lateOldIncarnation || !lateReplacementIncarnation ||
            lateOldIncarnation == lateReplacementIncarnation ||
            stats.groupedModules != 0 || stats.groupedFunctions != 0 ||
            stats.constructionSuccesses != 0) {
        testFail("MT WASM pending lifecycle rejects old-incarnation state after late replacement: replacement=%d same=%d jit=%d pendingFlag=%d pending=%u incarnations=%u/%u groups=%llu functions=%llu successes=%llu",
            replacement != nullptr,
            replacement && testContext().cpu->getOp(addresses[0], 0) == replacement,
            replacement && replacement->pfnJitCode != nullptr,
            replacement && (replacement->flags2 & OP_FLAG2_WASM_JIT_PENDING) != 0,
            wasmJitTestMtPendingCount(), lateOldIncarnation, lateReplacementIncarnation,
            stats.groupedModules, stats.groupedFunctions, stats.constructionSuccesses);
    }

    beginPhase(productionLimits, 160);
    addresses = addBlocks(2);
    wasmJitTestSetMtMappedFileKeyOverride(160);
    compile(testContext().cpu, addresses[0]);
    wasmJitTestSetMtMappedFileKeyOverride(161);
    compile(testContext().cpu, addresses[1]);
    testContext().memory->clearOpCache();
    if (wasmJitTestMtPendingCount() != 0) {
        testFail("MT WASM pending lifecycle clearOpCache cancels every pending entry");
    }

    beginPhase(productionLimits, 170);
    addresses = addBlocks(2);
    U32 oldIncarnation = wasmJitTestGetMtMemoryIncarnation(testContext().memory);
    wasmJitTestSetMtMappedFileKeyOverride(170);
    compile(testContext().cpu, addresses[0]);
    wasmJitTestSetMtMappedFileKeyOverride(171);
    compile(testContext().cpu, addresses[1]);
    testContext().memory->execvReset(false);
    bool execCancelled = wasmJitTestMtPendingCount() == 0;
    restoreMappings();
    U32 replacementIncarnation = wasmJitTestGetMtMemoryIncarnation(testContext().memory);
    replacement = testContext().cpu->getOp(addresses[0], 0);
    if (!execCancelled || !oldIncarnation || !replacementIncarnation ||
            replacementIncarnation == oldIncarnation || replacement->pfnJitCode ||
            (replacement->flags2 & OP_FLAG2_WASM_JIT_PENDING)) {
        testFail("MT WASM pending lifecycle exec retires old pending work before replacement");
    }

    beginPhase(productionLimits, 180);
    addresses = addBlocks(2);
    wasmJitTestSetMtMappedFileKeyOverride(180);
    compile(testContext().cpu, addresses[0]);
    wasmJitTestSetMtMappedFileKeyOverride(181);
    compile(testContext().cpu, addresses[1]);
    testContext().memory->cleanup();
    bool cleanupCancelled = wasmJitTestMtPendingCount() == 0;
    testContext().memory->execvReset(true);
    // Production exec resets the surviving CPU after replacing its memory.
    // This fixture bypasses KThread::reset(), so rebind its cached lookups here.
    testContext().cpu->reset();
    restoreMappings();
    if (!cleanupCancelled) {
        testFail("MT WASM pending lifecycle cleanup cancels every pending entry");
    }

    beginPhase(productionLimits, 190);
    addresses = addBlocks(2);
    DecodedOp* parentPending = compile(testContext().cpu, addresses[0]);
    KProcessPtr cloneProcess = KProcess::create();
    KMemory* cloneMemory = KMemory::create(cloneProcess.get());
    cloneProcess->memory = cloneMemory;
    cloneMemory->clone(testContext().memory, true);
    KThread* cloneThread = cloneProcess->createThread();
    cloneThread->cpu->clone(testContext().cpu);
    KThread::setCurrentThread(cloneThread);
    DecodedOp* clonePending = compile(cloneThread->cpu, addresses[1]);
    if (!(parentPending->flags2 & OP_FLAG2_WASM_JIT_PENDING) ||
            !(clonePending->flags2 & OP_FLAG2_WASM_JIT_PENDING) ||
            wasmJitTestMtPendingCount() != 2) {
        testFail("MT WASM pending lifecycle keeps CLONE_VM wrappers in separate batches");
    }
    clonePending->runCount = 0xff;
    for (U32 i = 0; i < productionLimits.urgentPendingHits; ++i) {
        runPending(cloneThread->cpu, addresses[1], clonePending);
    }
    if (!clonePending->pfnJitCode || clonePending->pfn != cloneProcess->startJITOp ||
            !(parentPending->flags2 & OP_FLAG2_WASM_JIT_PENDING) ||
            parentPending->pfnJitCode || wasmJitTestMtPendingCount() != 1) {
        testFail("MT WASM pending lifecycle preserves wrapper-local fallback and publication: cloneJit=%d clonePfn=%d parentPending=%d parentJit=%d pending=%u",
            clonePending->pfnJitCode != nullptr, clonePending->pfn == cloneProcess->startJITOp,
            (parentPending->flags2 & OP_FLAG2_WASM_JIT_PENDING) != 0,
            parentPending->pfnJitCode != nullptr, wasmJitTestMtPendingCount());
    }
    cloneMemory->removeCodeBlock(addresses[1], clonePending, false);
    cloneMemory->execvReset(true);
    KThread::setCurrentThread(testContext().thread);
    testContext().memory->removeCodeBlock(addresses[0], parentPending, false);
}
#else
void testWasmJitRuntimeGrouping() {
    if (!wasmJitTestRelocAllocationTransfer()) {
        testFail("cached persistence adopts lookup relocation allocation");
    }

    testNewInstruction(0);
    wasmJitTestResetRuntimeBatching();
    struct ResetBatching {
        ~ResetBatching() { wasmJitTestResetRuntimeBatching(); }
    } reset;
    WasmJitBatchLimits limits;
    limits.maxBlocks = 2;
    limits.maxBatchBytes = 1024 * 1024;
    limits.urgentPendingHits = 8;
    limits.maxProcessOpenBytes = 4 * 1024 * 1024;
    wasmJitTestSetBatchLimits(limits);
    wasmJitTestSetMappedFileKeyOverride(77);
    wasmJitTestEnableRuntimeBatching(true);

    U32 addresses[2];
    for (U32 i = 0; i < 2; ++i) {
        addresses[i] = testContext().codeIp;
        testPushCode8(i == 0 ? 0x40 : 0x41); // inc eax / inc ecx
        testPushCode8(0xcd);
        testPushCode8(0x97);
    }
    auto compile = [&](U32 address) {
        DecodedOp* op = testContext().cpu->getOp(address, 0);
        startNewJIT(testContext().cpu, address, op);
        return op;
    };
    DecodedOp* first = compile(addresses[0]);
    if (!(first->flags2 & OP_FLAG2_WASM_JIT_PENDING) || first->pfnJitCode) {
        testFail("runtime batch first block remains pending");
    }
    DecodedOp* second = compile(addresses[1]);
    if (!first->pfnJitCode || !second->pfnJitCode || first->pfnJitCode == second->pfnJitCode) {
        testFail("runtime batch publishes distinct table slots");
    }
    if (wasmJitTestRuntimeModuleCount() != 1 || wasmJitTestRuntimeGroupCount() != 1 || wasmJitTestPendingCount() != 0) {
        testFail("runtime batch instantiates one two-function module");
    }

    testContext().memory->removeCodeBlock(addresses[0], first, false);
    if (wasmJitTestRuntimeGroupCount() != 1) {
        testFail("runtime group survives first export release");
    }
    testContext().memory->removeCodeBlock(addresses[1], second, false);
    if (wasmJitTestRuntimeGroupCount() != 0 || wasmJitTestRuntimeGroupReleaseCount() != 1) {
        testFail("runtime group releases after final export");
    }

    WasmJitRuntimeStatsSnapshot stats = wasmJitTestGetRuntimeStats();
    if (stats.translatedFileBacked != 2 || stats.translatedAnonymous != 0 || stats.groupedModules != 1 || stats.groupedFunctions != 2 || stats.countFlushes != 1 || stats.standaloneModules != 0) {
        testFail("runtime batch aggregate statistics");
    }
    if (wasmJitTestRuntimeGroupReleaseCount() != 1) {
        testFail("runtime batch release statistics");
    }

    wasmJitTestResetRuntimeBatching();
    testNewInstruction(0);
    wasmJitTestSetBatchLimits(limits);
    wasmJitTestSetMappedFileKeyOverride(78);
    wasmJitTestEnableRuntimeBatching(true);
    wasmJitTestFailBatchAfterSlots(1);
    for (U32 i = 0; i < 2; ++i) {
        addresses[i] = testContext().codeIp;
        testPushCode8(0x40 + i);
        testPushCode8(0xcd);
        testPushCode8(0x97);
    }
    first = compile(addresses[0]);
    second = compile(addresses[1]);
    if (first->pfnJitCode || second->pfnJitCode || wasmJitTestPendingCount() != 0 || wasmJitTestRuntimeGroupCount() != 0) {
        testFail("runtime batch partial install rolls back every slot");
    }
    wasmJitTestFailBatchAfterSlots(-1);
}

void testWasmJitPendingLifecycle() {
    testNewInstruction(0);
    wasmJitTestResetRuntimeBatching();
    struct ResetBatching {
        ~ResetBatching() { wasmJitTestResetRuntimeBatching(); }
    } reset;

    WasmJitBatchLimits limits;
    limits.maxBlocks = 64;
    limits.maxBatchBytes = 1024 * 1024;
    limits.urgentPendingHits = 8;
    limits.maxProcessOpenBytes = 4 * 1024 * 1024;
    wasmJitTestSetBatchLimits(limits);
    wasmJitTestSetMappedFileKeyOverride(91);
    wasmJitTestEnableRuntimeBatching(true);

    U32 addresses[5];
    for (U32 i = 0; i < 5; ++i) {
        addresses[i] = testContext().codeIp;
        testPushCode8(0x40 + (i & 3)); // inc eax / inc ecx / inc edx / inc ebx
        testPushCode8(0xcd);
        testPushCode8(0x97);
    }
    U32 nestedAddress = testContext().codeIp;
    testPushCode8(0x40); // inc eax
    testPushCode8(0x41); // inc ecx
    testPushCode8(0x42); // inc edx
    testPushCode8(0xcd);
    testPushCode8(0x97);
    auto compile = [&](U32 address) {
        DecodedOp* op = testContext().cpu->getOp(address, 0);
        startNewJIT(testContext().cpu, address, op);
        return op;
    };

    DecodedOp* lifecycleHook = compile(addresses[4]);
    if (jitUsesCodeMemory()) {
        testFail("WASM JIT lifecycle owns installed entries outside code memory");
    }
    if (!(lifecycleHook->flags2 & OP_FLAG2_WASM_JIT_PENDING) || wasmJitTestPendingCount() != 1) {
        testFail("generic JIT lifecycle hook setup remains pending");
    }
    jitCodeInvalidated(testContext().memory, {lifecycleHook}, {});
    if ((lifecycleHook->flags2 & OP_FLAG2_WASM_JIT_PENDING) || wasmJitTestPendingCount() != 0) {
        testFail("generic JIT lifecycle hook cancels pending block");
    }
    testContext().memory->removeCodeBlock(addresses[4], lifecycleHook, false);

    DecodedOp* first = compile(addresses[0]);
    if (!(first->flags2 & OP_FLAG2_WASM_JIT_PENDING) || wasmJitTestPendingCount() != 1) {
        testFail("pending lifecycle range setup remains pending");
    }
    testContext().memory->removeCodeBlock(addresses[0], first, false);
    if (wasmJitTestPendingCount() != 0 || wasmJitTestRuntimeModuleCount() != 0) {
        testFail("pending lifecycle range invalidation cancels pending block");
    }

    DecodedOp* second = compile(addresses[1]);
    if (!(second->flags2 & OP_FLAG2_WASM_JIT_PENDING) || wasmJitTestPendingCount() != 1) {
        testFail("pending lifecycle full-cache setup remains pending");
    }
    testContext().memory->clearOpCache();
    if (wasmJitTestPendingCount() != 0 || wasmJitTestRuntimeModuleCount() != 0) {
        testFail("pending lifecycle full-cache invalidation cancels pending block");
    }

    DecodedOp* third = compile(addresses[2]);
    if (!(third->flags2 & OP_FLAG2_WASM_JIT_PENDING) || wasmJitTestPendingCount() != 1) {
        testFail("pending lifecycle urgent-hit setup remains pending");
    }
    for (U32 i = 0; i < 8; ++i) {
        testContext().cpu->eip.u32 = addresses[2] - testContext().cpu->seg[CS].address;
        testContext().cpu->nextOp = third;
        testContext().cpu->run();
    }
    bool missingMtRelocHazard = false;
#ifdef BOXEDWINE_MULTI_THREADED
    missingMtRelocHazard = !(third->flags2 & OP_FLAG2_WASM_JIT_RELOC_HAZARD);
#endif
    if (!third->pfnJitCode || (third->flags2 & OP_FLAG2_WASM_JIT_PENDING) ||
            missingMtRelocHazard ||
            wasmJitTestPendingCount() != 0 ||
            wasmJitTestRuntimeModuleCount() != 1 ||
            wasmJitTestRuntimeGroupCount() != 1) {
        testFail("pending lifecycle urgent hit installs one-entry runtime group");
    }
    testContext().memory->removeCodeBlock(addresses[2], third, false);

    DecodedOp* nestedFirst = compile(nestedAddress);
    DecodedOp* nestedSecond = nestedFirst->next;
    startNewJIT(testContext().cpu, nestedAddress + nestedFirst->len, nestedSecond);
    if (!(nestedFirst->flags2 & OP_FLAG2_WASM_JIT_PENDING) || !(nestedSecond->flags2 & OP_FLAG2_WASM_JIT_PENDING) || wasmJitTestPendingCount() != 2) {
        testFail("pending lifecycle nested entries remain pending");
    }
    testContext().memory->removeCodeBlock(nestedAddress, nestedFirst, false);
    if (wasmJitTestPendingCount() != 0) {
        testFail("pending lifecycle parent invalidation cancels nested entries");
    }

    auto restoreMappings = [&]() {
        constexpr U32 pagesPerSegment = 32;
        U32 segmentBytes = pagesPerSegment << K_PAGE_SHIFT;
        U32 stackBase = TEST_STACK_ADDRESS - segmentBytes;
        if (testContext().memory->mmap(testContext().thread, stackBase, segmentBytes, K_PROT_WRITE | K_PROT_READ, K_MAP_FIXED | K_MAP_PRIVATE, -1, 0) != stackBase || testContext().memory->mmap(testContext().thread, TEST_CODE_ADDRESS, segmentBytes, K_PROT_WRITE | K_PROT_READ | K_PROT_EXEC, K_MAP_FIXED | K_MAP_PRIVATE, -1, 0) != TEST_CODE_ADDRESS || testContext().memory->mmap(testContext().thread, TEST_HEAP_ADDRESS, segmentBytes, K_PROT_READ | K_PROT_WRITE, K_MAP_FIXED | K_MAP_PRIVATE, -1, 0) != TEST_HEAP_ADDRESS) {
            testFail("pending lifecycle restores mappings after memory reset");
        }
    };

    DecodedOp* fourth = compile(addresses[3]);
    if (!(fourth->flags2 & OP_FLAG2_WASM_JIT_PENDING) || wasmJitTestPendingCount() != 1) {
        testFail("pending lifecycle exec-reset setup remains pending");
    }
    testContext().memory->execvReset(false);
    if (wasmJitTestPendingCount() != 0) {
        testFail("pending lifecycle exec reset cancels pending block");
    }
    restoreMappings();

    U32 teardownAddress = testContext().codeIp;
    testPushCode8(0x43); // inc ebx
    testPushCode8(0xcd);
    testPushCode8(0x97);
    DecodedOp* teardownOp = compile(teardownAddress);
    if (!(teardownOp->flags2 & OP_FLAG2_WASM_JIT_PENDING) || wasmJitTestPendingCount() != 1) {
        testFail("pending lifecycle teardown setup remains pending");
    }
    // Published code of the exiting process too: nothing could reach it again. Catches its slots and group staying
    // in the table and JS for the rest of the session.
    U32 publishedAddress = testContext().codeIp;
    testPushCode8(0x41); // inc ecx
    testPushCode8(0x42); // inc edx
    testPushCode8(0x43); // inc ebx
    testPushCode8(0x46); // inc esi
    testPushCode8(0xcd);
    testPushCode8(0x97);
    DecodedOp* publishedOp = compile(publishedAddress);
    for (U32 i = 0; i < limits.urgentPendingHits && !publishedOp->pfnJitCode; ++i) {
        wasmJitHandlePendingHit(testContext().cpu, publishedOp);
    }
    if (!publishedOp->pfnJitCode || wasmJitTestRuntimeGroupCount() == 0) {
        testFail("pending lifecycle teardown setup publishes a runtime group");
    }
    U32 groupsReleased = wasmJitTestRuntimeGroupReleaseCount();
    testContext().memory->cleanup();
    bool teardownCancelledPending = wasmJitTestPendingCount() == 0;
    bool teardownFreedCode = wasmJitTestRuntimeGroupCount() == 0 && wasmJitTestRuntimeGroupReleaseCount() > groupsReleased;
    testContext().memory->execvReset(true);
    // Match the CPU reset that follows memory replacement in production exec.
    testContext().cpu->reset();
    restoreMappings();
    if (!teardownCancelledPending) {
        testFail("pending lifecycle teardown cancels pending block");
    }
    if (!teardownFreedCode) {
        testFail("process exit frees the process's published JIT code");
    }
}

void testWasmJitTinyAnonymousPromotion() {
    testNewInstruction(0);
    wasmJitTestResetRuntimeBatching();
    struct ResetBatching {
        ~ResetBatching() { wasmJitTestResetRuntimeBatching(); }
    } reset;

    WasmJitBatchLimits limits;
    limits.maxBlocks = 64;
    limits.maxBatchBytes = 1024 * 1024;
    limits.urgentPendingHits = 2;
    limits.maxProcessOpenBytes = 4 * 1024 * 1024;
    wasmJitTestSetBatchLimits(limits);
    wasmJitTestSetMappedFileKeyOverride(0);
    wasmJitTestSetTinyAdditionalHits(2);
    wasmJitTestEnableRuntimeBatching(true);

    U32 tinyAddress = testContext().codeIp;
    testPushCode8(0x40); // inc eax
    testPushCode8(0xcd);
    testPushCode8(0x97);

    U32 normalAddress = testContext().codeIp;
    testPushCode8(0x40); // inc eax
    testPushCode8(0x41); // inc ecx
    testPushCode8(0x42); // inc edx
    testPushCode8(0x43); // inc ebx
    testPushCode8(0xcd);
    testPushCode8(0x97);

    U32 invalidatedAddress = testContext().codeIp;
    testPushCode8(0x43); // inc ebx
    testPushCode8(0xcd);
    testPushCode8(0x97);

    U32 oomTinyAddress = testContext().codeIp;
    testPushCode8(0x42); // inc edx
    testPushCode8(0xcd);
    testPushCode8(0x97);

    auto compile = [&](U32 address) {
        DecodedOp* op = testContext().cpu->getOp(address, 0);
        startNewJIT(testContext().cpu, address, op);
        return op;
    };
    auto isStandaloneJit = [&](DecodedOp* op) {
        return op && op->pfn == testContext().cpu->thread->process->startJITOp && op->pfnJitCode && (op->flags & OP_FLAG_JIT) && !(op->flags2 & OP_FLAG2_WASM_JIT_PENDING);
    };

    DecodedOp* tiny = compile(tinyAddress);
    if (!tiny || !(tiny->flags2 & OP_FLAG2_WASM_JIT_PENDING) || tiny->pfnJitCode || wasmJitTestPendingCount() != 1 || wasmJitTestPendingRawBytes() == 0) {
        testFail("tiny anonymous block retains standalone bytes while pending");
        return;
    }
    wasmJitHandlePendingHit(testContext().cpu, tiny);
    if (!(tiny->flags2 & OP_FLAG2_WASM_JIT_PENDING) || tiny->pfnJitCode || wasmJitTestPendingCount() != 1) {
        testFail("tiny anonymous block remains pending before promotion target");
    }
    wasmJitHandlePendingHit(testContext().cpu, tiny);
    if (!isStandaloneJit(tiny) || wasmJitTestPendingCount() != 0 || wasmJitTestRuntimeGroupCount() != 0) {
        testFail("tiny anonymous block promotes through standalone instantiation");
    }

    DecodedOp* normal = compile(normalAddress);
    if (!normal || !(normal->flags2 & OP_FLAG2_WASM_JIT_PENDING) || normal->pfnJitCode || wasmJitTestPendingCount() != 1) {
        testFail("four-op anonymous block waits in its address space's runtime batch");
        return;
    }
    wasmJitHandlePendingHit(testContext().cpu, normal);
    wasmJitHandlePendingHit(testContext().cpu, normal);
    if (!isStandaloneJit(normal) || wasmJitTestPendingCount() != 0 || wasmJitTestRuntimeGroupCount() != 1) {
        testFail("four-op anonymous block installs through a runtime batch group");
    }

    DecodedOp* invalidated = compile(invalidatedAddress);
    if (!invalidated || !(invalidated->flags2 & OP_FLAG2_WASM_JIT_PENDING) || wasmJitTestPendingCount() != 1 || wasmJitTestPendingRawBytes() == 0) {
        testFail("tiny anonymous invalidation setup retains pending bytes");
    }
    testContext().memory->removeCodeBlock(invalidatedAddress, invalidated, false);
    if (wasmJitTestPendingCount() != 0 || wasmJitTestPendingRawBytes() != 0) {
        testFail("tiny anonymous invalidation releases retained bytes");
    }

    DecodedOp* oomTiny = compile(oomTinyAddress);
    if (!oomTiny || !(oomTiny->flags2 & OP_FLAG2_WASM_JIT_PENDING) || oomTiny->pfnJitCode || wasmJitTestPendingCount() != 1) {
        testFail("tiny anonymous OOM setup remains pending");
        return;
    }
    auto runPending = [&](U32 address, DecodedOp* op) {
        testContext().cpu->eip.u32 = address - testContext().cpu->seg[CS].address;
        testContext().cpu->nextOp = op;
        testContext().cpu->run();
    };
    U32 attemptsBeforeOom = wasmJitTestStandaloneModuleAttemptCount();
    boxedwine_wasm_test_force_next_module_oom();
    runPending(oomTinyAddress, oomTiny);
    runPending(oomTinyAddress, oomTiny);
    if (!(oomTiny->flags2 & OP_FLAG2_WASM_JIT_PENDING) || oomTiny->pfnJitCode || wasmJitTestPendingCount() != 1 || wasmJitTestSealedCount() != 1 || !wasmJitCompilationPaused() || wasmJitTestStandaloneModuleAttemptCount() != attemptsBeforeOom + 1) {
        testFail("tiny anonymous OOM retains one globally ordered sealed request");
    }

    U32 pausedAttempts = wasmJitTestStandaloneModuleAttemptCount();
    runPending(oomTinyAddress, oomTiny);
    if (wasmJitTestStandaloneModuleAttemptCount() != pausedAttempts || !(oomTiny->flags2 & OP_FLAG2_WASM_JIT_PENDING) || !wasmJitCompilationPaused()) {
        testFail("tiny anonymous OOM blocks firstDynamicOp retry before release");
    }

    boxedwine_wasm_test_force_next_module_oom();
    testContext().memory->removeCodeBlock(normalAddress, normal, false);
    if (isStandaloneJit(oomTiny) || wasmJitTestPendingCount() != 1 || wasmJitTestSealedCount() != 1 || !wasmJitCompilationPaused() || wasmJitTestStandaloneModuleAttemptCount() != pausedAttempts + 1) {
        testFail("tiny anonymous retry OOM reseals until another real release");
    }

    U32 retryOomAttempts = wasmJitTestStandaloneModuleAttemptCount();
    runPending(oomTinyAddress, oomTiny);
    if (wasmJitTestStandaloneModuleAttemptCount() != retryOomAttempts) {
        testFail("tiny anonymous repeated OOM remains paused before another release");
    }

    testContext().memory->removeCodeBlock(tinyAddress, tiny, false);
    if (!isStandaloneJit(oomTiny) || wasmJitTestPendingCount() != 0 || wasmJitTestSealedCount() != 0 || wasmJitCompilationPaused() || wasmJitTestStandaloneModuleAttemptCount() != retryOomAttempts + 1) {
        testFail("tiny anonymous OOM retries and publishes after a real slot release");
    }

    testContext().memory->removeCodeBlock(oomTinyAddress, oomTiny, false);
}

// Catches: a pause lasting until some slot happens to be freed (an idle screen frees none, compiling stays off for the
// rest of the session), a retry per tick instead of per probe period, and requests behind a retried one staying sealed
// (they also hold back the hot groups) until yet another release.
void testWasmJitOomProbe() {
    testNewInstruction(0);
    wasmJitTestResetRuntimeBatching();
    boxedwine_wasm_test_reset_oom_state();
    struct ResetBatching {
        ~ResetBatching() {
            wasmJitTestResetRuntimeBatching();
            boxedwine_wasm_test_reset_oom_state();
        }
    } reset;
    WasmJitBatchLimits limits;
    limits.maxBlocks = 2;
    limits.maxBatchBytes = 1024 * 1024;
    limits.urgentPendingHits = 8;
    limits.maxProcessOpenBytes = 4 * 1024 * 1024;
    wasmJitTestSetBatchLimits(limits);
    wasmJitTestSetMappedFileKeyOverride(101);
    wasmJitTestEnableRuntimeBatching(true);

    U32 addresses[4];
    for (U32 i = 0; i < 4; ++i) {
        addresses[i] = testContext().codeIp;
        testPushCode8(0x40 + i);
        testPushCode8(0xcd);
        testPushCode8(0x97);
    }
    DecodedOp* ops[4];
    auto compile = [&](U32 i) {
        ops[i] = testContext().cpu->getOp(addresses[i], 0);
        startNewJIT(testContext().cpu, addresses[i], ops[i]);
    };
    auto installed = [&](U32 i) {
        return ops[i]->pfnJitCode && !(ops[i]->flags2 & OP_FLAG2_WASM_JIT_PENDING);
    };
    boxedwine_wasm_test_force_next_module_oom();
    for (U32 i = 0; i < 4; ++i) {
        compile(i);
    }
    U32 attempts = wasmJitTestRuntimeModuleAttemptCount();
    if (!wasmJitCompilationPaused() || wasmJitTestSealedCount() != 2 || wasmJitTestPendingCount() != 4) {
        testFail("OOM probe setup: two sealed groups and a pause");
    }
    double start = 1000;
    wasmJitTestOomTick(start); // the tick that first sees the pause starts its period
    wasmJitTestOomTick(start + 4999);
    if (wasmJitTestRuntimeModuleAttemptCount() != attempts || !wasmJitCompilationPaused()) {
        testFail("OOM probe: no attempt before the period ends");
    }
    boxedwine_wasm_test_force_next_module_oom();
    wasmJitTestOomTick(start + 5000);
    if (wasmJitTestRuntimeModuleAttemptCount() != attempts + 1 || !wasmJitCompilationPaused() || wasmJitTestSealedCount() != 2) {
        testFail("OOM probe: a failed retry pauses again and keeps the requests");
    }
    wasmJitTestOomTick(start + 9999);
    if (wasmJitTestRuntimeModuleAttemptCount() != attempts + 1) {
        testFail("OOM probe: a failed retry waits for the next period");
    }
    wasmJitTestOomTick(start + 10000);
    if (wasmJitTestRuntimeModuleAttemptCount() != attempts + 2 || wasmJitCompilationPaused() || !installed(0) || !installed(1) || wasmJitTestSealedCount() != 1) {
        testFail("OOM probe: the retry publishes the oldest request with no slot released");
    }
    wasmJitTestOomTick(start + 10001); // not paused: the next request needs no wait
    if (wasmJitTestRuntimeModuleAttemptCount() != attempts + 3 || !installed(2) || !installed(3) || wasmJitTestSealedCount() != 0 || wasmJitTestPendingCount() != 0) {
        testFail("OOM probe: requests behind the retried one follow without a release");
    }
    for (U32 i = 0; i < 4; ++i) {
        testContext().memory->removeCodeBlock(addresses[i], ops[i], false);
    }
}

void testWasmJitGroupedOomRecovery() {
    testNewInstruction(0);
    wasmJitTestResetRuntimeBatching();
    boxedwine_wasm_test_reset_oom_state();
    struct ResetBatching {
        ~ResetBatching() {
            wasmJitTestResetRuntimeBatching();
            boxedwine_wasm_test_reset_oom_state();
        }
    } reset;

    WasmJitBatchLimits limits;
    limits.maxBlocks = 2;
    limits.maxBatchBytes = 1024 * 1024;
    limits.urgentPendingHits = 8;
    limits.maxProcessOpenBytes = 4 * 1024 * 1024;
    wasmJitTestSetBatchLimits(limits);
    wasmJitTestSetMappedFileKeyOverride(0);
    wasmJitTestSetTinyAdditionalHits(1);
    wasmJitTestEnableRuntimeBatching(true);

    // A tiny anonymous block promotes to a standalone slot (other anonymous blocks batch).
    U32 releasableAddress = testContext().codeIp;
    testPushCode8(0x40); // inc eax
    testPushCode8(0xcd);
    testPushCode8(0x97);

    U32 addresses[2];
    for (U32 i = 0; i < 2; ++i) {
        addresses[i] = testContext().codeIp;
        testPushCode8(0x40 + i);
        testPushCode8(0xcd);
        testPushCode8(0x97);
    }

    auto compile = [&](U32 address) {
        DecodedOp* op = testContext().cpu->getOp(address, 0);
        startNewJIT(testContext().cpu, address, op);
        return op;
    };
    auto isInstalled = [&](DecodedOp* op) {
        return op && op->pfn == testContext().cpu->thread->process->startJITOp && op->pfnJitCode && (op->flags & OP_FLAG_JIT) && !(op->flags2 & OP_FLAG2_WASM_JIT_PENDING);
    };

    DecodedOp* releasable = compile(releasableAddress);
    wasmJitHandlePendingHit(testContext().cpu, releasable);
    if (!isInstalled(releasable)) {
        testFail("grouped OOM setup creates a releasable standalone slot");
        return;
    }

    wasmJitTestSetMappedFileKeyOverride(101);
    boxedwine_wasm_test_force_next_module_oom();
    DecodedOp* first = compile(addresses[0]);
    DecodedOp* second = compile(addresses[1]);
    if (!(first->flags2 & OP_FLAG2_WASM_JIT_PENDING) || first->pfnJitCode || !(second->flags2 & OP_FLAG2_WASM_JIT_PENDING) || second->pfnJitCode || wasmJitTestPendingCount() != 2 || wasmJitTestSealedCount() != 1) {
        testFail("grouped OOM retains one sealed two-entry runtime batch");
    }

    U32 attempts = wasmJitTestRuntimeModuleAttemptCount();
    wasmJitHandlePendingHit(testContext().cpu, first);
    if (wasmJitTestRuntimeModuleAttemptCount() != attempts) {
        testFail("grouped OOM blocks repeated module attempts before release");
    }

    testContext().memory->removeCodeBlock(addresses[0], first, false);
    if (wasmJitTestPendingCount() != 1 || wasmJitTestSealedCount() != 1) {
        testFail("grouped OOM cancellation rebuilds the retained sealed batch");
    }

    testContext().memory->removeCodeBlock(releasableAddress, releasable, false);
    if (!isInstalled(second) || wasmJitTestPendingCount() != 0 || wasmJitTestSealedCount() != 0 || wasmJitTestRuntimeModuleAttemptCount() != attempts + 1 || wasmJitTestRuntimeModuleCount() != 1 || wasmJitTestRuntimeGroupCount() != 1) {
        testFail("grouped OOM release retries the surviving entry as one runtime group");
    }
    testContext().memory->removeCodeBlock(addresses[1], second, false);
}
#endif
#endif
