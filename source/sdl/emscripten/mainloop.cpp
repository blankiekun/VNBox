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

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include "boxedwine.h"
#include "knativesocket.h"
#include "kbrowsersocket.h"
#include "knativesystem.h"
#include "vnboxstats.h"
#include "../../x11/x11.h"
#include <algorithm>
#include "kdspaudio_math.h"

#ifdef BOXEDWINE_MULTI_THREADED
#include "knativethread.h"

U32 getNextTimer();
void runTimers();

extern std::atomic<int> platformThreadCount;

static U32 lastTitleUpdate = 0;
static thread_local bool isMainThread;

extern int allocatedRamPages;

bool isMainthread() {
    return isMainThread;
}

static BString getSize(int pages)
{
    pages *= 4;
    if (pages < 2048) {
        return BString::valueOf(pages) + B("KB");
    }
    if (pages < 2048 * 1024) {
        return BString::valueOf(pages / 1024) + B("MB");
    }
    return BString::valueOf(pages / 1024 / 1024) + B("GB");
}
extern int allocatedRamPages;
void mainloop() {
    isMainThread = true;
    checkBrowserSocketEvents();
        U32 t = KSystem::getMilliesSinceStart();
        U32 nextTimer = getNextTimer();
        if (nextTimer == 0) {
            runTimers();
        }
           
        if (lastTitleUpdate + 5000 < t) {
            lastTitleUpdate = t;
            BString title;
            if (KSystem::title.length()) {
                title = KSystem::title;
            } else {
                title = B("BoxedWine " BOXEDWINE_VERSION_DISPLAY);
            }

            title.append(" ");
            title.append(getSize(allocatedRamPages));
	    emscripten_set_window_title(title.c_str());
        }
#ifdef BOXEDWINE_RECORDER
        BOXEDWINE_RECORDER_RUN_SLICE();
#endif
        if (!KNativeSystem::getCurrentInput()->processEvents()) {
            KNativeSystem::cleanup();
        }
}

void waitForProcessToFinish(const std::shared_ptr<KProcess>& process, KThread* thread) {
    BOXEDWINE_CRITICAL_SECTION_WITH_CONDITION(KSystem::processesCond);
    while (!process->isTerminated()) {
        BOXEDWINE_CONDITION_WAIT(KSystem::processesCond);
    }
}

#else

static U32 lastTitleUpdate = 0;
static int mainLoopTimingMode = -1;
static int mainLoopTimingValue = -1;
#ifdef BOXEDWINE_JSPI
static int jspiTickDelay = -1; // what boxedwine_tick returns: ms to wait before the next tick, -1 = next message
#endif
static U32 idleWaitMs = 16; // the idleWait= URL param: longest idle wait while no guest timer is due sooner
static U32 wakeGridMs = 8; // the wakeGrid= URL param: idle wakes land on this ms grid, so guest timers due close together share one wake

static void updateMainLoopTiming(bool idle) {
    int mode = EM_TIMING_SETTIMEOUT;
    int value = 1;
#ifdef BOXEDWINE_WASM_JIT
    // Keep yielding to the browser between slices without paying the nested
    // timer delay while guest work is available. Sleeping guests still use a
    // timer so they do not continuously post messages to the browser.
    if (!idle) {
        mode = EM_TIMING_SETIMMEDIATE;
    }
#endif
    if (idle) {
        // Browsers hold a timeout set from inside another timeout for at least 4 ms,
        // which would run a guest timer due sooner than that late, such as one of an
        // audio driver's 10 ms periods. A timeout set after a message is not held,
        // so wait for such a timer with one message and then one timeout.
        U32 nextTimer = getNextTimer();
        if (nextTimer == 0 || (nextTimer < 4 && wakeGridMs < 2)) {
            if (nextTimer == 0 || mainLoopTimingMode == EM_TIMING_SETTIMEOUT) {
                mode = EM_TIMING_SETIMMEDIATE;
            } else {
#ifdef BOXEDWINE_JSPI
                value = (int)nextTimer; // straight to setTimeout, which never fires early
#else
                value = (int)nextTimer + 1; // emscripten rounds the delay down
#endif
            }
        } else {
            // Sleep until the next guest timer (getNextTimer is taken after the tick, so the tick's
            // own run time is already left out), at most idleWaitMs: a sleeping guest used to be
            // woken every 1 ms. Input wakes the JSPI loop at once (doMainLoop).
            value = (int)std::min(nextTimer, idleWaitMs);
            if (wakeGridMs > 1) { // timer slack: wake at the grid line at or after the timer, late by less than one grid step
                U32 now = KSystem::getMilliesSinceStart();
                value = (int)((now + value + wakeGridMs - 1) / wakeGridMs * wakeGridMs - now);
            }
        }
        // a frame the fps cap held back is drawn by the next tick once it is due (not while hidden: nothing is drawn)
        XServer* server = XServer::getServer(true);
        if (server && server->isDisplayDirty && server->getRoot() && XServer::drawFps && !XServer::hidden && mode == EM_TIMING_SETTIMEOUT) {
            U64 now = KSystem::getMicroCounter();
            int due = XServer::nextDrawUs > now ? (int)((XServer::nextDrawUs - now + 999) / 1000) : 0;
            value = std::min(value, due);
        }
    }
#ifdef BOXEDWINE_JSPI
    jspiTickDelay = mode == EM_TIMING_SETTIMEOUT ? value : -1;
    mainLoopTimingMode = mode;
    mainLoopTimingValue = value;
    return;
#endif
    if (mode != mainLoopTimingMode || (mode == EM_TIMING_SETTIMEOUT && value != mainLoopTimingValue)) {
        emscripten_set_main_loop_timing(mode, mode == EM_TIMING_SETTIMEOUT ? value : 0);
        mainLoopTimingMode = mode;
        mainLoopTimingValue = value;
    }
}

static constexpr U32 MIPS_WINDOW = 20;
static U32 mipsSamples[MIPS_WINDOW] = {};
static U32 mipsSampleIndex = 0;
static U32 mipsSampleCount = 0;
static U32 mipsLogCount = 0;
static U64 mipsWarmTotal = 0;
static U32 mipsWarmCount = 0;

static U32 recordMipsSample(U32 mips, U32* minOut, U32* maxOut) {
    mipsSamples[mipsSampleIndex] = mips;
    mipsSampleIndex = (mipsSampleIndex + 1) % MIPS_WINDOW;
    if (mipsSampleCount < MIPS_WINDOW) {
        mipsSampleCount++;
    }
    U64 total = 0;
    U32 minVal = 0xffffffffu;
    U32 maxVal = 0;
    for (U32 i = 0; i < mipsSampleCount; ++i) {
        U32 s = mipsSamples[i];
        total += s;
        if (s < minVal) minVal = s;
        if (s > maxVal) maxVal = s;
    }
    if (minOut) *minOut = minVal;
    if (maxOut) *maxOut = maxVal;
    return mipsSampleCount ? (U32)((total + mipsSampleCount / 2) / mipsSampleCount) : mips;
}

bool isMainthread() {
    return true;
}

void mainloop() {
    vnboxStatsTick(mainLoopTimingMode == EM_TIMING_SETTIMEOUT ? mainLoopTimingValue : -1); // stats=1 / prof=1
    U64 startTime = KSystem::getMicroCounter();
    checkBrowserSocketEvents();
    bool idle = false;
    U32 t;
    U32 count=0;
    BString mipsTitle;
    while (1) {
        bool ran;
        try {
            ran = runSlice();
        } catch (...) {
            if (!recoverRunSliceException()) {
                throw;
            }
            break;
        }

        KNativeSystem::tick();
#ifdef BOXEDWINE_RECORDER
        BOXEDWINE_RECORDER_RUN_SLICE();
#endif
#ifdef BOXEDWINE_MULTI_THREADED
        checkWaitingNativeSockets(0);
#endif
        if (!KNativeSystem::getCurrentInput()->processEvents()) {
            KNativeSystem::cleanup();
            return;
        }
        t = KSystem::getMilliesSinceStart();                
        if (lastTitleUpdate+1000 < t) {
            lastTitleUpdate = t;
            U32 mipsRaw = getMIPS();
            vnboxStatsMips = mipsRaw;
            U32 mipsMin = 0, mipsMax = 0;
            U32 mipsAvg = recordMipsSample(mipsRaw, &mipsMin, &mipsMax);
            mipsLogCount++;
            if (mipsLogCount >= MIPS_WINDOW) {
                mipsWarmTotal += mipsRaw;
                mipsWarmCount++;
            }
            U32 mipsWarmAvg = mipsWarmCount ? (U32)((mipsWarmTotal + mipsWarmCount / 2) / mipsWarmCount) : mipsAvg;
            if (mipsLogCount % 10 == 0) { // sample every second, log every 10th
                klog_fmt("[MIPS] count=%u raw=%u avg=%u min=%u max=%u warmAvg=%u warmSamples=%u",
                    mipsLogCount, mipsRaw, mipsAvg, mipsMin, mipsMax, mipsWarmAvg, mipsWarmCount);
            }
	    mipsTitle = B("BoxedWine ");
	    mipsTitle.append(mipsAvg);
	    mipsTitle.append(" MIPS");
	    emscripten_set_window_title(mipsTitle.c_str());
        }
        if (!ran) {
            idle = true;
            break;
        }
        U64 diff = KSystem::getMicroCounter() - startTime;

        KOpenGLPtr openGL = KNativeSystem::getOpenGL();
        if (diff>10000 || KNativeSystem::getScreen()->presentedSinceLastCheck() || (openGL && openGL->presentedSinceLastCheck())) {
            if (diff > 100000) {
                klog_fmt("ran main loop in %dms", (U32)diff / 1000);
            }
            break;
        }
    };
    static bool exited = false;
    // Running, not just listed: processes nobody waits on (the first one, Wine's services after
    // wineserver quits) stay listed as zombies, so the list never empties.
    if (!exited && KSystem::getRunningProcessCount() == 0) {
        // every guest process has exited (an installer finished): tell a page embedding the player
        exited = true;
        klog("all processes have exited");
        EM_ASM({ try { if (parent !== window) parent.postMessage({ type: "vnbox-exit" }, location.origin); } catch (e) { } });
    }
    updateMainLoopTiming(idle);
    vnboxStatsTickEnd(idle);
}

#endif

// A whole-number URL param of the player page (vnboxParam, lazyfs.js), clamped to [lo, hi]; dflt when absent.
EM_JS(int, vnboxUrlInt, (const char* name, int dflt, int lo, int hi), {
    var v = parseInt(vnboxParam(UTF8ToString(name)), 10);
    return isNaN(v) ? dflt : Math.min(hi, Math.max(lo, v));
});

bool doMainLoop() {
    // Smoothness knobs, read before the guest runs. audioPeriod: frames per SDL audio callback
    // (the non-worklet audio path).
#ifndef BOXEDWINE_MULTI_THREADED
    idleWaitMs = vnboxUrlInt("idleWait", idleWaitMs, 1, 1000);
    wakeGridMs = vnboxUrlInt("wakeGrid", wakeGridMs, 0, 50);
#endif
    XServer::drawFps = vnboxUrlInt("fps", XServer::drawFps, 0, 240); // frames shown per second at most, 0 = unlocked
    U32 period = vnboxUrlInt("audioPeriod", KDspAudioMath::sdlPeriodFrames, 256, 16384);
    KDspAudioMath::sdlPeriodFrames = 1u << (31 - __builtin_clz(period)); // a power of two, as SDL wants
    EM_ASM(
#ifndef SDL2
            SDL.defaults.copyOnLock = false;
            SDL.defaults.discardOnLock = true;
#endif
            //SDL.defaults.opaqueFrontBuffer = false;
    );
#ifdef BOXEDWINE_JSPI
    // JSPI needs every entry into wasm that may suspend to be a promising export, which
    // emscripten's main loop callback is not: drive mainloop from JS through one, never
    // re-entering while a tick is suspended, paced like updateMainLoopTiming paces
    // emscripten's loop (a message while the guest has work, a timeout while it sleeps).
    // Firefox runs a chain of messages ahead of painting (one frame per ~80 ms while the
    // guest is busy, during movies and loads): once a frame is due, the next tick waits for it.
    // Input during an idle wait runs the next tick right after the event's handlers have queued it.
    // Frames are only watched while the guest is busy: an idle page asks for none, so the browser can sleep between ticks.
    // A hidden page gets no frames: the guest and audio keep running, nothing is drawn (vnbox_set_hidden),
    // a tick waiting for a frame goes on at once, and the next tick once visible draws the latest frame.
    EM_ASM({
        var tick = WebAssembly.promising(wasmExports["boxedwine_tick"]);
        var channel = new MessageChannel();
        var lastFrame = performance.now();
        var busy = false; var watching = false;
        var frame = function() { lastFrame = performance.now(); if (busy) requestAnimationFrame(frame); else watching = false; };
        var post = function() { channel.port2.postMessage(0); };
        var timer = 0;
        var wait = function(delay) { timer = setTimeout(function() { timer = 0; next(); }, delay); };
        var wake = function() { if (timer) { clearTimeout(timer); wait(0); } };
        var raf = 0; // the frame a tick waits for, which never comes once the page is hidden
        var framed = function() { raf = 0; post(); };
        var visibility = function() {
            wasmExports["vnbox_set_hidden"](document.hidden ? 1 : 0);
            if (!document.hidden) wake();
            else if (raf) { cancelAnimationFrame(raf); framed(); }
        };
        document.addEventListener("visibilitychange", visibility);
        visibility(); // a player opened in a background tab
        "keydown keyup mousedown mouseup mousemove wheel touchstart touchend touchmove".split(" ").forEach(function(type) {
            window.addEventListener(type, wake, true);
        });
        var next = function() { tick().then(function(delay) {
            busy = delay < 0;
            if (busy && !watching) { watching = true; lastFrame = performance.now(); requestAnimationFrame(frame); } // a busy stretch starts with a fresh frame budget
            if (delay >= 0) wait(delay);
            else if (!document.hidden && performance.now() - lastFrame > 12) raf = requestAnimationFrame(framed);
            else post();
        }, function(e) { if (typeof vnboxReport === "function") vnboxReport("tick", e); else console.error("tick failed: " + e); }); };
        channel.port1.onmessage = next;
        next();
    });
    emscripten_exit_with_live_runtime();
#else
    emscripten_set_main_loop(mainloop, 0, 1);
#endif
    return false;
}
#ifdef BOXEDWINE_JSPI
void cdromRunQueuedInsert();
extern "C" EMSCRIPTEN_KEEPALIVE void vnbox_set_hidden(int hidden) {
    XServer::hidden = hidden != 0;
}
extern "C" EMSCRIPTEN_KEEPALIVE int boxedwine_tick() {
    cdromRunQueuedInsert();
    mainloop();
    return jspiTickDelay;
}
#endif
#endif
