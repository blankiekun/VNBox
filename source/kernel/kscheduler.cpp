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
#include "../x11/x11.h"
#include "knativeaudio.h"
#include "vnboxstats.h"
#include "vnboxidle.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include <chrono>
#endif

#ifdef BOXEDWINE_MULTI_THREADED
static KList<KTimerCallback*> timers;
static BOXEDWINE_MUTEX timerMutex;
void runTimers() {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(timerMutex);
    U32 millies = KSystem::getMilliesSinceStart();
    timers.for_each([millies](KListNode<KTimerCallback*>* node) {
        KTimerCallback* timer = node->data;

        if (timer->millies <= millies) {
            if (timer->run()) {
                timer->node.remove();
            }
        }
        });
}

U32 getNextTimer() {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(timerMutex);
    U32 millies = KSystem::getMilliesSinceStart();
    U32 result = 0xFFFFFFFF;

    timers.for_each([millies, &result](KListNode<KTimerCallback*>* node) {
        KTimerCallback* timer = node->data;
        U32 next = 0;

        if (timer->millies <= millies) {
            next = 0;
        }
        else {
            next = timer->millies - millies;
        }
        if (next < result) {
            result = next;
        }
        });
    return result;
}

void addTimer(KTimerCallback* timer) {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(timerMutex);
    timers.addToBack(&timer->node);
    timer->active = true;
}

void removeTimer(KTimerCallback* timer) {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(timerMutex);
    timer->node.remove();
    timer->active = false;
}
#else
#include "kscheduler.h"
#include "knativesystem.h"

#include <stdio.h>

//#define LOG_SCHEDULER

KList<KThread*> scheduledThreads;
KList<KThread*> waitThreads;
KList<KTimerCallback*> timers;

void addTimer(KTimerCallback* timer) {
    timers.addToBack(&timer->node);
    timer->active = true;
}

void removeTimer(KTimerCallback* timer) {
    timer->node.remove();
    timer->active = false;
}

static U64 schedulerMicroCounter();

void scheduleThread(KThread* thread) {
#ifdef _DEBUG
    if (thread->waitingCond) {
        kpanic("can't schedule a thread that is waiting");
    }
    if (thread->condTimer.active) {
        kpanic("can't schedule a thread that is waiting on a timer");
    }
#endif
#ifdef __EMSCRIPTEN__
    if (thread->hotBlockedSinceUs) { // hot-thread throttle: how long it waited
        thread->hotBlockedUs += schedulerMicroCounter() - thread->hotBlockedSinceUs;
        thread->hotBlockedSinceUs = 0;
    }
#endif
    thread->cpu->yield = false;
    scheduledThreads.addToFront(&thread->scheduledThreadNode);
}

void unscheduleThread(KThread* thread) {	    
    thread->scheduledThreadNode.remove();
    thread->cpu->yield = true;
}

void terminateOtherThread(const KProcessPtr& process, U32 threadId) {
    KThread* thread = process->getThreadById(threadId);
    if (thread) {
        unscheduleThread(thread);
        process->deleteThread(thread);
    }
}

void terminateCurrentThread(KThread* thread) {
	thread->terminating = true;
	unscheduleThread(thread);
}

// Instruction batches bound JIT dispatch within each timed thread turn. Keep
// their adaptation bounded on every host; elapsed time determines fairness.
static const S32 DEFAULT_CONTEXT_TIME = 10000;
static const S32 MIN_CONTEXT_TIME = 2000;
static const S32 MAX_CONTEXT_TIME = 100000;
static const S32 CONTEXT_TIME_STEP = 10000;
S32 contextTime = DEFAULT_CONTEXT_TIME;
S32 contextTimeRemaining = DEFAULT_CONTEXT_TIME;
int count;
extern struct Block emptyBlock;

#ifdef __TEST
static U64 (*schedulerTestClock)() = nullptr;

void setSchedulerTestClock(U64 (*clock)()) {
    schedulerTestClock = clock;
}
#endif

static U64 schedulerMicroCounter() {
#ifdef __TEST
    if (schedulerTestClock) {
        return schedulerTestClock();
    }
#endif
    // Scheduling uses monotonic host time, independent of the guest's clock
    // adjustment and changes to the system date/time.
#ifdef __EMSCRIPTEN__
    return (U64)(emscripten_get_now() * 1000.0);
#else
    return (U64)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

static S32 decreaseContextTime(S32 value) {
    if (value <= MIN_CONTEXT_TIME + CONTEXT_TIME_STEP) {
        return MIN_CONTEXT_TIME;
    }
    return value - CONTEXT_TIME_STEP;
}

static S32 increaseContextTime(S32 value) {
    if (value >= MAX_CONTEXT_TIME - CONTEXT_TIME_STEP) {
        return MAX_CONTEXT_TIME;
    }
    return value + CONTEXT_TIME_STEP;
}

#ifdef __EMSCRIPTEN__
static KThread* runSliceExceptionThread;

bool recoverRunSliceException() {
    if (!runSliceExceptionThread) {
        return false;
    }
    runSliceExceptionThread->cpu->nextOp = nullptr;
    runSliceExceptionThread = nullptr;
    return true;
}
#ifdef BOXEDWINE_HOST_WINED3D
static KThread* runSliceSavedThread;

// A trap in host wined3d unwound the slice without its destructors (source/wined3d/thunkHost.cpp
// vnbox_wd3d_recover): what ChangeThread and the catch in mainloop would have done.
bool recoverRunSliceTrap() {
    if (!runSliceExceptionThread) {
        return false;
    }
    KThread::setCurrentThread(runSliceSavedThread);
    return recoverRunSliceException();
}
#endif
#endif

void runThreadSlice(KThread* thread) {
    CPU* cpu;

    cpu = thread->cpu;
    if (thread->ptraceStopped) {
        cpu->yield = true;
        return;
    }
    cpu->blockInstructionCount = 0;
    cpu->yield = false;
    cpu->nextOp = cpu->getNextOp(); // another thread that just ran could have modified this
    // Guest threads can execute very different numbers of instructions
    // per millisecond. Share execution time rather than giving a fast thread
    // only a small fraction of the time used by a slower busy-polling thread.
    const U64 sliceEnd = schedulerMicroCounter() + 1000;
#ifdef __EMSCRIPTEN__
    // Browser clock calls are costly for short dispatches. Check the
    // same timed deadline in instruction batches, but honor every yield below.
    U32 nextClockCheck = 1024;
#endif
    contextTimeRemaining = contextTime;
#ifndef __EMSCRIPTEN__
    try {
#endif
    do {
        cpu->run();
#ifdef __TEST
        if (cpu->nextOp && cpu->nextOp->inst == TestEnd) {
            break;
        }
#endif
        if (cpu->yield) {
            break;
        }
#ifdef __EMSCRIPTEN__
        if (cpu->blockInstructionCount < nextClockCheck) {
            continue;
        }
        nextClockCheck = cpu->blockInstructionCount + 1024;
#endif
        if (schedulerMicroCounter() >= sliceEnd) {
            break;
        }
        if ((S32)cpu->blockInstructionCount >= contextTimeRemaining) {
            contextTimeRemaining = cpu->blockInstructionCount + contextTime;
        }
    } while (true);
#ifndef __EMSCRIPTEN__
    } catch (...) {
        cpu->nextOp = nullptr;
    }
#endif

    cpu->instructionCount+=cpu->blockInstructionCount;
}

void runTimers() {
    U32 millies = KSystem::getMilliesSinceStart();
    timers.for_each([millies] (KListNode<KTimerCallback*>* node) {
        KTimerCallback* timer = node->data;

        if (timer->millies<=millies) {
            vnboxStatTimer((double)(millies - timer->millies));
            if (timer->run()) {                
                timer->node.remove();
            }
        }
    });
}

extern U64 sysCallTime;
U64 elapsedTimeMIPS;
U64 elapsedInstructionsMIPS;

U64 vnboxSliceLimitUs = 9000; // guest time one runSlice may use (the main loop lowers it under a CPU cap)

bool KThreadThrottleTimer::run() {
    active = false;
    // a signal may have run the thread early; it may be waiting on something else by now
    if (!thread->terminating && !thread->waitingCond && !thread->ptraceStopped && !thread->scheduledThreadNode.isInList()) {
        scheduleThread(thread);
    }
    return true;
}

#if defined(__EMSCRIPTEN__)
// Hot-thread throttle (vnboxidle.h): called at the end of a thread's turn in which it put a frame. A thread is hot
// when it was runnable at least half the time since its previous frame (a busy loop, Sleep(0) included, not a game
// that sleeps or waits between frames; a CPU cap holding the guest doesn't count as waiting). A hot thread whose
// frames change nothing that matters (vnboxIdleNow: none of the idle cap's wake reasons lately) sleeps, 1 ms after
// the first such frame, twice as long after each next one, up to hotThrottle= ms. Any wake reason or a frame drawn
// without busy-looping starts the ramp over. The sleep is a real one: guest clocks keep time. Rate alone never
// throttles: a busy game whose screen changes (or that presents with GL) runs at full speed.
static bool hotThrottle(KThread* thread, U64 now) {
    bool hot = thread->hotSinceUs && now > thread->hotSinceUs && thread->hotBlockedUs * 2 <= now - thread->hotSinceUs;
    thread->hotBlockedUs = 0;
    thread->hotSinceUs = now;
    if (!hot || !vnboxHotThrottleMaxMs || !vnboxIdleNow()) {
        thread->hotThrottleMs = 0;
        return false;
    }
    U32 ms = thread->hotThrottleMs ? std::min(thread->hotThrottleMs * 2, vnboxHotThrottleMaxMs) : 1;
    thread->hotThrottleMs = ms;
    thread->hotSinceUs = now + ms * 1000ull; // the sleep isn't the thread's idle time
    if (thread->hotTimer.active) { // a signal ran it before its sleep was over
        removeTimer(&thread->hotTimer);
    }
    thread->hotTimer.thread = thread;
    thread->hotTimer.millies = KSystem::getMilliesSinceStart() + ms;
    addTimer(&thread->hotTimer);
    if (vnboxStats.on) {
        vnboxStats.hotThrottles++;
        vnboxStats.hotThrottleMs += ms;
        if (thread->hotStatsLog != vnboxStatsLogs + 1) {
            thread->hotStatsLog = vnboxStatsLogs + 1;
            vnboxStats.hotThreads++;
        }
    }
    return true;
}
#endif

bool runSlice() {    
    runTimers();

    XServer* server = XServer::getServer(true);
    if (server) {
        // draws only when something changed (XWindow::setDirty, configure, a held-back present);
        // upstream forced a composite every slice here for missed Basstour popups (setDirty's early return)
        double drawStart = vnboxStatNow();
        server->draw();
        if (!scheduledThreads.isEmpty()) vnboxStatDraw(drawStart); // slice-start calls, as before idle ticks drew
    }
    if (scheduledThreads.isEmpty())
        return false;

    U64 elapsedTime = 0;
    const U64 sliceStart = schedulerMicroCounter();
    U64 now = sliceStart; // one clock read per thread turn: each turn starts where the last ended
    U64 nextTimerCheck = 1000;

    while (!scheduledThreads.isEmpty() && elapsedTime<vnboxSliceLimitUs) {
        // Yielding threads can stay runnable while waiting for a sleeping
        // worker. Service its timer during the slice, not just every 9 ms.
        if (elapsedTime >= nextTimerCheck) {
            runTimers();
            nextTimerCheck = elapsedTime + 1000;
            if (scheduledThreads.isEmpty()) {
                break;
            }
            // A frame due mid-slice is shown now, not at the next slice (fps=60/90/120 would
            // land on ~10 ms slice boundaries); ending the slice lets the browser paint it.
            if (server && XServer::drawFps && server->vnboxDrawShown()) {
                break;
            }
            now = schedulerMicroCounter(); // timers and draws aren't the thread's time
        }
        U64 threadStartTime = now;
        KListNode<KThread*>* node = scheduledThreads.front();
        KThread* currentThread = (KThread*)node->data;
        KNativeSystem::scheduledNewThread(currentThread);
        sysCallTime = 0;

#ifdef __EMSCRIPTEN__
        runSliceExceptionThread = currentThread;
#ifdef BOXEDWINE_HOST_WINED3D
        runSliceSavedThread = KThread::currentThread();
#endif
#endif
        ChangeThread c(currentThread);
        static U64 rdtsc;
        currentThread->cpu->instructionCount = rdtsc;
        runThreadSlice(currentThread);
#ifdef __EMSCRIPTEN__
        runSliceExceptionThread = nullptr;
#endif
        rdtsc = currentThread->cpu->instructionCount;

        now = schedulerMicroCounter();
        U64 diff = now - threadStartTime;

        elapsedTime = now - sliceStart;

        elapsedTimeMIPS+=diff;
        vnboxStatSlice(currentThread, diff, sysCallTime);
        elapsedInstructionsMIPS+=currentThread->cpu->blockInstructionCount;

        bool throttle = false;
#if defined(__EMSCRIPTEN__)
        if (vnboxFramePutThread == currentThread) {
            vnboxFramePutThread = nullptr;
            throttle = !currentThread->terminating && !currentThread->ptraceStopped && !currentThread->waitingCond && hotThrottle(currentThread, now);
        }
        if (currentThread->waitingCond) {
            currentThread->hotBlockedSinceUs = now; // scheduleThread adds the time when it runs again
        }
#endif
        currentThread->userTime+=diff-sysCallTime;
        currentThread->kernelTime+=sysCallTime;

        // this is how we signal to delete the current thread, since we can't delete it in the syscall, maybe we should use smart_ptr for threads
        if (currentThread->terminating) {
            KProcessPtr process = currentThread->process;
            process->deleteThread(currentThread);
        } else if (currentThread->ptraceStopped) {
            node->remove();
        } else if (!currentThread->waitingCond) {
            // make sure we are behind any threads that were recently scheduled
            node->remove();
            if (!throttle) { // a throttled thread comes back from its timer (hotTimer)
                scheduledThreads.addToBack(node);
            }
        }
    }
    if (!scheduledThreads.isEmpty() && vnboxSliceLimitUs >= 9000) { // a capped slice is short on purpose
        if (elapsedTime>11000) {
            contextTime = decreaseContextTime(contextTime);
        } else if (elapsedTime<9500) {
            contextTime = increaseContextTime(contextTime);
        }
    }
    //klog("ran slice in %dus %d", (U32)elapsedTime, contextTime);    
    
    return true;
}

U32 getMIPS() {
    U32 result = 0;
    if (elapsedTimeMIPS) {
        result = (U32)(elapsedInstructionsMIPS/elapsedTimeMIPS);
        elapsedTimeMIPS = 0;
        elapsedInstructionsMIPS = 0;
    }
    return result;
}

U32 getNextTimer() {
    U32 now = KSystem::getMilliesSinceStart();
    U32 result = 0xffffffff;
    timers.for_each([now, &result](KListNode<KTimerCallback*>* node) {
        U32 deadline = node->data->millies;
        U32 remaining = deadline <= now ? 0 : deadline - now;
        if (remaining < result) {
            result = remaining;
        }
    });
    return result;
}

void waitForProcessToFinish(const KProcessPtr& process, KThread* thread) {
    while (!process->terminated) {
        runThreadSlice(thread);
    }
}
#endif
