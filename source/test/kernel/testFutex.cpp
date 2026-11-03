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

#include "../cpu/testCPU.h"

#include "devntsync.h"
#include "ksignal.h"
#include "kstat.h"
#include "../../io/fsvirtualnode.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace {

constexpr U32 TEST_FUTEX_ADDRESS = TEST_HEAP_ADDRESS + 0x980;
constexpr U32 TEST_FUTEX_WAIT = 0;
constexpr U32 TEST_FUTEX_WAKE = 1;
constexpr U32 TEST_FUTEX_PRIVATE_FLAG = 128;
constexpr U32 TEST_FUTEX_VALUE = 0x12345678;

bool waitForFutexWorker(const std::atomic<bool>& done, U32 timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done.load(std::memory_order_acquire);
}

} // namespace

void testSharedFutexUnmappedAddressReturnsEFAULT() {
    TestContext& context = testContext();
    constexpr U32 address = 0x61000000;
    if (context.memory->mmap(context.thread, address, K_PAGE_SIZE,
        K_PROT_READ | K_PROT_WRITE, K_MAP_FIXED | K_MAP_PRIVATE, -1, 0) != address) {
        testFail("map shared-futex test page");
        return;
    }
    context.memory->writed(address, TEST_FUTEX_VALUE);
    if (context.thread->futex(address, TEST_FUTEX_WAKE, 1, 0, 0, 0, false) != 0) {
        testFail("mapped shared futex wake without waiters must return zero");
    }
    if (context.memory->unmap(address, K_PAGE_SIZE) != 0) {
        testFail("unmap shared-futex test page");
        return;
    }
    for (bool time64 : {false, true}) {
        for (U32 command : {TEST_FUTEX_WAIT, TEST_FUTEX_WAKE}) {
            U32 result = context.thread->futex(address, command, TEST_FUTEX_VALUE,
                0, 0, 0, time64);
            if (result != (U32)-K_EFAULT) {
                testFail("unmapped shared futex command %u time64 %d expected EFAULT, got %d",
                    command, time64, (S32)result);
            }
        }
    }
}

// Catches: thread exit never walking the robust list (NULL list_op_pending ended it, as it always is), ignoring
// futex_offset, or marking a lock the dying thread doesn't hold.
void testRobustListOnThreadExit() {
    TestContext& context = testContext();
    constexpr U32 PAGE = 0x61400000;
    constexpr U32 HEAD = PAGE;
    constexpr U32 LOCK_OFFSET = 20; // glibc's i386 pthread_mutex_t: __list is 20 bytes after __lock
    constexpr U32 HELD = PAGE + 0x100;
    constexpr U32 NOT_HELD = PAGE + 0x200;
    constexpr U32 PENDING = PAGE + 0x300;
    constexpr U32 WAITERS = 0x80000000;
    constexpr U32 OWNER_DIED = 0x40000000;
    KMemory* memory = context.memory;
    if (memory->mmap(context.thread, PAGE, K_PAGE_SIZE, K_PROT_READ | K_PROT_WRITE, K_MAP_FIXED | K_MAP_PRIVATE, -1, 0) != PAGE) {
        testFail("map robust list page");
        return;
    }
    for (bool withPending : {false, true}) {
        KThread* dying = context.process->createThread();
        memory->writed(HEAD, HELD + LOCK_OFFSET);
        memory->writed(HEAD + 4, (U32)-(S32)LOCK_OFFSET);
        memory->writed(HEAD + 8, withPending ? PENDING + LOCK_OFFSET : 0);
        memory->writed(HELD + LOCK_OFFSET, NOT_HELD + LOCK_OFFSET);
        memory->writed(NOT_HELD + LOCK_OFFSET, HEAD);
        memory->writed(HELD, dying->id | WAITERS);
        memory->writed(NOT_HELD, (dying->id + 1) | WAITERS);
        memory->writed(PENDING, dying->id);
        if (dying->set_robust_list(HEAD, 12) != 0) {
            testFail("set_robust_list");
        }
        context.process->deleteThread(dying);
        if (memory->readd(HELD) != (OWNER_DIED | WAITERS)) {
            testFail("held robust lock after its owner died: 0x%X", memory->readd(HELD));
        }
        if ((memory->readd(NOT_HELD) & OWNER_DIED) != 0) {
            testFail("a robust lock someone else holds was marked: 0x%X", memory->readd(NOT_HELD));
        }
        if ((memory->readd(PENDING) == OWNER_DIED) != withPending) {
            testFail("pending robust lock (listed %d): 0x%X", withPending, memory->readd(PENDING));
        }
    }
    memory->unmap(PAGE, K_PAGE_SIZE);
}

void testTerminatingThreadDoesNotEnterFutexWait() {
#ifdef BOXEDWINE_MULTI_THREADED
    TestContext& context = testContext();
    KThread* mainThread = context.thread;
    KThread* waiter = context.process->createThread();
    std::atomic<bool> done(false);
    std::atomic<U32> result(0);

    context.memory->writed(TEST_FUTEX_ADDRESS, TEST_FUTEX_VALUE);
    waiter->terminating = true;

    std::thread worker([waiter, &done, &result]() {
        KThread::setCurrentThread(waiter);
        result.store(waiter->futex(TEST_FUTEX_ADDRESS, TEST_FUTEX_WAIT | TEST_FUTEX_PRIVATE_FLAG, TEST_FUTEX_VALUE, 0, 0, 0, false), std::memory_order_release);
        done.store(true, std::memory_order_release);
        KThread::setCurrentThread(nullptr);
    });

    if (!waitForFutexWorker(done, 100)) {
        KThread::setCurrentThread(mainThread);
        mainThread->futex(TEST_FUTEX_ADDRESS, TEST_FUTEX_WAKE | TEST_FUTEX_PRIVATE_FLAG, 1, 0, 0, 0, false);
        waitForFutexWorker(done, 1000);
        testFail("terminating thread entered futex wait before returning EINTR");
    }

    worker.join();
    KThread::setCurrentThread(mainThread);

    const U32 expected = -K_EINTR;
    if (result.load(std::memory_order_acquire) != expected) {
        testFail("terminating futex wait expected %d (0x%X), got %d (0x%X)", (S32)expected, expected, (S32)result.load(std::memory_order_acquire), result.load(std::memory_order_acquire));
    }

    context.process->deleteThread(waiter);
#endif
}

#ifndef BOXEDWINE_MULTI_THREADED
// Catches: a thread that takes a signal during WAIT_ANY staying queued on the event, as it was before PR 107, so the
// next set is claimed for it and the thread still waiting sleeps on. Linux takes it out of the wait for the handler.
void testNtsyncSignalDuringWaitLeavesEvent() {
    TestContext& context = testContext();
    constexpr U32 PAGE = 0x61600000;
    constexpr U32 EVENT_ARGS = PAGE;
    constexpr U32 OBJS = PAGE + 0x10;
    constexpr U32 WAIT1 = PAGE + 0x40;
    constexpr U32 WAIT2 = PAGE + 0x80;
    constexpr U32 INDEX_OFFSET = 20;
    constexpr U32 STACK = PAGE + K_PAGE_SIZE / 2;
    constexpr U32 NO_INDEX = 0xABAB;
    constexpr U32 HANDLER = 0x2000;
    constexpr U32 IOC_CREATE_EVENT = 0x40084e87;
    constexpr U32 IOC_WAIT_ANY = 0xc0284e82;
    constexpr U32 IOC_EVENT_SET = 0x80044e88;
    constexpr U32 IOC_EVENT_READ = 0x80084e8d;
    KMemory* memory = context.memory;
    if (memory->mmap(context.thread, PAGE, K_PAGE_SIZE, K_PROT_READ | K_PROT_WRITE, K_MAP_FIXED | K_MAP_PRIVATE, -1, 0) != PAGE) {
        testFail("map ntsync test page");
        return;
    }
    KProcess* process = context.process.get();
    std::shared_ptr<FsNode> node = std::make_shared<FsVirtualNode>(0, k_mdev(10, 124), B("/dev/ntsync"), openDevNtsync,
        K__S_IWRITE | K__S_IREAD | K__S_IFCHR, nullptr, 0, 0, 0);
    FsOpenNode* device = openDevNtsync(node, K_O_RDWR, 0);
    auto ioctl = [](KThread* thread, auto* target, U32 request, U32 arg) {
        ChangeThread c(thread);
        thread->cpu->reg[2].u32 = arg; // IOCTL_ARG1
        return target->ioctl(thread, request);
    };
    // manual-reset off, not signaled
    memory->writed(EVENT_ARGS, 0);
    memory->writed(EVENT_ARGS + 4, 0);
    const U32 eventFd = ioctl(context.thread, device, IOC_CREATE_EVENT, EVENT_ARGS);
    KFileDescriptorPtr event = process->getFileDescriptor(eventFd);
    if (!event) {
        testFail("create ntsync event: %d", (S32)eventFd);
        memory->unmap(PAGE, K_PAGE_SIZE);
        return;
    }
    memory->writed(OBJS, eventFd);
    for (U32 args : {WAIT1, WAIT2}) {
        memory->writeq(args, ~0ULL); // no timeout
        memory->writeq(args + 8, OBJS);
        memory->writed(args + 16, 1); // count
        memory->writed(args + INDEX_OFFSET, NO_INDEX);
        memory->writed(args + 24, 0); // flags
        memory->writed(args + 28, 1); // owner
        memory->writed(args + 32, 0); // alert
        memory->writed(args + 36, 0); // pad
    }
    KThread* signaled = process->createThread();
    KThread* other = process->createThread();
    if (ioctl(signaled, device, IOC_WAIT_ANY, WAIT1) != (U32)-K_WAIT || ioctl(other, device, IOC_WAIT_ANY, WAIT2) != (U32)-K_WAIT) {
        testFail("both threads must block on the unset event");
    }
    const KSigAction savedAction = process->sigActions[K_SIGUSR1];
    process->sigActions[K_SIGUSR1].handlerAndSigAction = HANDLER;
    process->sigActions[K_SIGUSR1].flags = 0;
    signaled->cpu->reg[4].u32 = STACK;
    signaled->signal(K_SIGUSR1, false);
    process->sigActions[K_SIGUSR1] = savedAction;
    if (signaled->cpu->eip.u32 != HANDLER) {
        testFail("the handler must be entered: eip 0x%X", signaled->cpu->eip.u32);
    }

    ioctl(context.thread, event->kobject.get(), IOC_EVENT_SET, PAGE + 0x20);
    const U32 otherResult = ioctl(other, device, IOC_WAIT_ANY, WAIT2);
    if (otherResult != 0 || memory->readd(WAIT2 + INDEX_OFFSET) != 0) {
        testFail("the thread still waiting must get the event: result %d, index 0x%X", (S32)otherResult, memory->readd(WAIT2 + INDEX_OFFSET));
    }
    if (memory->readd(WAIT1 + INDEX_OFFSET) != NO_INDEX) {
        testFail("the thread in its handler must not be handed the event");
    }
    ioctl(context.thread, event->kobject.get(), IOC_EVENT_READ, PAGE + 0x28);
    if (memory->readd(PAGE + 0x2c) != 0) {
        testFail("the auto-reset event must be taken once");
    }
    process->deleteThread(signaled);
    process->deleteThread(other);
    process->close(eventFd);
    delete device;
    memory->unmap(PAGE, K_PAGE_SIZE);
}
#endif

#endif
