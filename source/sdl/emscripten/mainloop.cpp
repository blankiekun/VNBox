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
#include "vnboxwinereq.h"
#include <algorithm>
#include <map>
#include <tuple>
#include <string>
#include <string.h>
#include <vector>
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
        if (nextTimer < 4) {
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
        }
        // a frame the fps cap held back is drawn by the next tick once it is due
        XServer* server = XServer::getServer(true);
        if (server && server->isDisplayDirty && server->getRoot() && XServer::drawFps && mode == EM_TIMING_SETTIMEOUT) {
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

static double statsLastTickEnd = 0;
static double statsLastLog = 0;
static U32 statsLastMips = 0;

// stats=1: the browser side of the line (JIT module compiles, lazy game/zip file reads), taken and reset per window.
EM_JS(void, statsJsPart, (char* out, int len, double sec), {
    var j = Module.vnboxJit || { ms: 0, n: 0 };
    Module.vnboxJit = { ms: 0, n: 0 };
    var s = " | jitCompile ms/s=" + (j.ms / sec).toFixed(0) + " modules/s=" + (j.n / sec).toFixed(1);
    var l = globalThis.__lazyStats;
    if (l) {
        var w = l.win || { reads: 0, bytes: 0, ms: 0, fetches: 0, fetchBytes: 0, files: {} };
        l.win = { reads: 0, bytes: 0, ms: 0, fetches: 0, fetchBytes: 0, files: {} };
        var top = Object.entries(w.files).sort(function(a, b) { return b[1] - a[1]; }).slice(0, 3)
            .map(function(e) { return e[0] + " " + e[1].toFixed(0) + "ms"; }).join(", ");
        s += " | lazy wait ms/s=" + (w.ms / sec).toFixed(0) + " reads/s=" + (w.reads / sec).toFixed(0) +
            " KB/s=" + (w.bytes / 1024 / sec).toFixed(0) + " avgRead=" + (w.reads ? w.bytes / 1024 / w.reads : 0).toFixed(1) + "KB" +
            " fetches/s=" + (w.fetches / sec).toFixed(1) + " fetchKB/s=" + (w.fetchBytes / 1024 / sec).toFixed(0) + " top: " + (top || "-");
    }
    stringToUTF8(s, out, len);
});

static const char* statsSyscallName(U32 n) { // i386 numbers; the ones a Wine game tends to spend time in
    switch (n) {
    case 3: return "read"; case 4: return "write"; case 5: return "open"; case 6: return "close"; case 19: return "lseek";
    case 33: return "access"; case 45: return "brk"; case 54: return "ioctl"; case 77: return "getrusage"; case 78: return "gettimeofday";
    case 90: return "mmap"; case 91: return "munmap"; case 102: return "socketcall"; case 125: return "mprotect";
    case 140: return "llseek"; case 142: return "select"; case 145: return "readv"; case 146: return "writev";
    case 158: return "sched_yield"; case 162: return "nanosleep"; case 168: return "poll"; case 174: return "rt_sigaction";
    case 175: return "rt_sigprocmask"; case 180: return "pread64"; case 181: return "pwrite64"; case 192: return "mmap2";
    case 195: return "stat64"; case 196: return "lstat64"; case 197: return "fstat64"; case 220: return "getdents64";
    case 221: return "fcntl64"; case 240: return "futex"; case 256: return "epoll_wait"; case 265: return "clock_gettime";
    case 295: return "openat"; case 300: return "fstatat64"; case 308: return "pselect6"; case 309: return "ppoll";
    case 319: return "epoll_pwait"; case 383: return "statx"; case 403: return "clock_gettime64"; case 407: return "clock_nanosleep64";
    case 413: return "pselect6_64"; case 414: return "ppoll64"; case 422: return "futex64";
    default: return nullptr;
    }
}

// The 5 syscalls with the most ms in this window: "name count/ms, ...".
static BString statsTopSyscalls(const VnboxStats& s) {
    BString out;
    bool used[440] = {};
    for (int k = 0; k < 5; k++) {
        int best = -1;
        for (int i = 0; i < 440; i++) {
            if (!used[i] && s.sysCount[i] && (best < 0 || s.sysMsBy[i] > s.sysMsBy[best])) best = i;
        }
        if (best < 0) break;
        used[best] = true;
        const char* name = statsSyscallName(best);
        char item[64];
        char blocked[24] = "";
        if (s.sysBlocked[best]) snprintf(blocked, sizeof(blocked), " (%u blocked)", s.sysBlocked[best]);
        if (name) snprintf(item, sizeof(item), "%s%s %ux/%.0fms%s", k ? ", " : "", name, s.sysCount[best], s.sysMsBy[best], blocked);
        else snprintf(item, sizeof(item), "%s#%d %ux/%.0fms%s", k ? ", " : "", best, s.sysCount[best], s.sysMsBy[best], blocked);
        out += item;
    }
    return out.length() ? out : B("-");
}

static const char* statsWineRequestName(unsigned req) {
    return req < vnboxWineRequestCount ? vnboxWineRequestNames[req] : "?";
}

// wineserver requests sent in this window: the 6 most frequent per second, then the 3 busiest sending threads
// (process:thread id) with their 3 most frequent requests.
static BString statsWineRequests(const VnboxStats& s, double sec) {
    const unsigned n = sizeof(s.wineReq) / sizeof(s.wineReq[0]);
    BString out;
    unsigned total = 0;
    for (unsigned i = 0; i < n; i++) total += s.wineReq[i];
    char item[160];
    snprintf(item, sizeof(item), "sent/s=%.0f top:", total / sec);
    out += item;
    std::vector<std::pair<unsigned, unsigned>> reqs; // count, request
    for (unsigned i = 0; i < n; i++) if (s.wineReq[i]) reqs.push_back({s.wineReq[i], i});
    std::sort(reqs.rbegin(), reqs.rend());
    for (size_t k = 0; k < reqs.size() && k < 6; k++) {
        snprintf(item, sizeof(item), "%s %s %.0f", k ? "," : "", statsWineRequestName(reqs[k].second), reqs[k].first / sec);
        out += item;
    }
    std::unordered_map<unsigned long long, unsigned> threads; // process << 24 | thread -> count
    for (auto& e : s.wineReqByThread) threads[e.first >> 16] += e.second;
    std::vector<std::pair<unsigned, unsigned long long>> busiest;
    for (auto& e : threads) busiest.push_back({e.second, e.first});
    std::sort(busiest.rbegin(), busiest.rend());
    out += " | reqBy:";
    for (size_t k = 0; k < busiest.size() && k < 3; k++) {
        unsigned long long key = busiest[k].second;
        unsigned pid = (unsigned)(key >> 24), tid = (unsigned)(key & 0xFFFFFF);
        auto name = s.processNames.find(pid);
        snprintf(item, sizeof(item), "%s %s:%u/%u %.0f (", k ? ";" : "", name != s.processNames.end() ? name->second.c_str() : "?", pid, tid, busiest[k].first / sec);
        out += item;
        std::vector<std::pair<unsigned, unsigned>> mine;
        for (auto& e : s.wineReqByThread) if ((e.first >> 16) == key) mine.push_back({e.second, (unsigned)(e.first & 0xFFFF)});
        std::sort(mine.rbegin(), mine.rend());
        for (size_t j = 0; j < mine.size() && j < 3; j++) {
            snprintf(item, sizeof(item), "%s%s %.0f", j ? ", " : "", statsWineRequestName(mine[j].second), mine[j].first / sec);
            out += item;
        }
        out += ")";
    }
    return out;
}

// The 4 threads with the most guest slice time in this window: "process:pid/tid ms/s (slices/s)".
static BString statsBusyThreads(const VnboxStats& s, double sec) {
    std::vector<std::pair<double, unsigned long long>> busiest;
    for (auto& e : s.threadMs) busiest.push_back({e.second, e.first});
    std::sort(busiest.rbegin(), busiest.rend());
    BString out;
    char item[128];
    for (size_t k = 0; k < busiest.size() && k < 4; k++) {
        unsigned long long key = busiest[k].second;
        unsigned pid = (unsigned)(key >> 24), tid = (unsigned)(key & 0xFFFFFF);
        auto name = s.processNames.find(pid);
        auto runs = s.threadRuns.find(key);
        snprintf(item, sizeof(item), "%s %s:%u/%u %.0fms (%.0f slices)", k ? "," : "", name != s.processNames.end() ? name->second.c_str() : "?", pid, tid,
            busiest[k].first / sec, runs != s.threadRuns.end() ? runs->second / sec : 0);
        out += item;
    }
    return out.length() ? out : B(" -");
}

// stats=1: how a tick waited (asked vs real), then every ~2 s one console line with the counters.
static void statsTickStart() {
    double now = emscripten_get_now();
    if (statsLastTickEnd) {
        double gap = now - statsLastTickEnd;
        int asked = mainLoopTimingMode == EM_TIMING_SETTIMEOUT ? mainLoopTimingValue : -1;
        if (asked >= 0) {
            double late = gap - asked;
            vnboxStats.tickWaits++;
            vnboxStats.tickLateMs += late;
            if (late > vnboxStats.tickLateMaxMs) vnboxStats.tickLateMaxMs = late;
        } else {
            vnboxStats.msgWaits++;
            vnboxStats.msgGapMs += gap;
            if (gap > vnboxStats.msgGapMaxMs) vnboxStats.msgGapMaxMs = gap;
        }
    }
    if (!statsLastLog) {
        statsLastLog = now;
    } else if (now - statsLastLog >= 2000) {
        VnboxStats& s = vnboxStats;
        double sec = (now - statsLastLog) / 1000.0;
        // instructions the guest ran; syscalls also add their time as made-up instructions (kept out of the share)
        double guestInstr = s.instr > s.sysPadInstr ? (double)(s.instr - s.sysPadInstr) : 0;
        char js[512];
        statsJsPart(js, sizeof(js), sec);
        klog_fmt("[stats] %.1fs mips=%u guestBusy=%.0f%% ticks busy=%u idle=%u | timeoutWaits=%u late avg=%.1f max=%.1fms | msg/rAF waits=%u gap avg=%.1f max=%.1fms"
            " | guestTimers=%u late avg=%.1f max=%.0fms | glPresents/s=%.1f ms/present=%.2f | xDraws/s=%.0f ms/draw=%.2f shown/s=%.1f skippedSame/s=%.1f busyDropped/s=%.1f composites/s=%.1f compositeMs/s=%.0f swPresentMs/s=%.0f"
            " | glCalls/s=%.0f glGetError/s=%.0f glMs/s=%.0f readbacks/s=%.1f readbackMs/s=%.0f readbackKB/s=%.0f readPixels/s=%.1f readPixelsMs/s=%.0f | input/s=%.1f motion/s=%.0f"
            " | sys ms/s=%.0f user ms/s=%.0f | threads/s:%s | topSys: %s | jit instr=%.0f%% (interp %.0f%%) entries/s=%.0f blocksCompiled/s=%.0f"
            " | wineserver ms/s=%.0f req/s=%.0f | wineReq %s | signals/s=%.1f faults/s=%.1f | codePageWrites/s=%.0f codeRemovedByWrite/s=%.1f noJitDynamicOps/s=%.1f clock_nanosleep64/s=%.1f | audio underruns/s=%.1f queuedMin=%.0fms | zipMiss/s=%.1f zipMs/s=%.0f%s",
            sec, statsLastMips, s.sliceMs / sec / 10.0, s.ticksBusy, s.ticksIdle,
            s.tickWaits, s.tickWaits ? s.tickLateMs / s.tickWaits : 0, s.tickLateMaxMs,
            s.msgWaits, s.msgWaits ? s.msgGapMs / s.msgWaits : 0, s.msgGapMaxMs,
            s.timers, s.timers ? s.timerLateMs / s.timers : 0, s.timerLateMaxMs,
            s.presentsGl / sec, s.presentsGl ? s.presentGlMs / s.presentsGl : 0, s.draws / sec, s.draws ? s.drawMs / s.draws : 0,
            (s.presentsSw + s.presentsGl) / sec, s.skippedSame / sec, s.busyDropped / sec, s.composites / sec, s.compositeMs / sec, s.presentSwMs / sec,
            s.glCalls / sec, s.glGetErrors / sec, s.glMs / sec, s.glReadbacks / sec, s.glReadbackMs / sec, s.glReadbackKB / sec, s.glReadPixels / sec, s.glReadPixelsMs / sec, s.inputs / sec, s.motions / sec,
            s.sysMs / sec, (s.sliceMs - s.sysMs) / sec, statsBusyThreads(s, sec).c_str(), statsTopSyscalls(s).c_str(),
            guestInstr ? 100.0 * s.jitInstr / guestInstr : 0, guestInstr ? 100.0 - 100.0 * s.jitInstr / guestInstr : 0,
            s.jitEntries / sec, s.blocksCompiled / sec,
            s.wineserverMs / sec, s.wineserverRequests / sec, statsWineRequests(s, sec).c_str(), s.signals / sec, s.faults / sec,
            s.codePageWrites / sec, s.codeRemovedByWrite / sec, s.opsNoJitDynamic / sec, s.sysCount[407] / sec, s.audioUnderruns / sec, s.audioQueuedMinMs, s.zipMisses / sec, s.zipMs / sec, js);
        s = VnboxStats();
        s.on = true;
        statsLastLog = now;
    }
}

// prof=1: every ~5 s print the guest JIT time by module+offset (this window), then the cumulative top blocks as JSON.
struct ProfAgg { double ms = 0; unsigned long long instr = 0, entries = 0; };
static void profReport(double now) {
    static double last = 0;
    static std::map<std::tuple<std::string, std::string, unsigned>, ProfAgg> cum;
    if (!last) { last = now; return; }
    if (now - last < 5000) return;
    double sec = (now - last) / 1000.0;
    last = now;
    std::map<std::tuple<std::string, std::string, unsigned>, ProfAgg> win;
    std::map<std::string, double> byModule;
    double total = 0;
    for (auto& e : vnboxProfBlocks) {
        unsigned pid = (unsigned)(e.first >> 32), eip = (unsigned)e.first;
        KProcessPtr p = KSystem::getProcess(pid);
        std::string pname = p ? std::string(p->name.c_str()) : "gone";
        std::string mod = "?";
        unsigned off = eip;
        if (p) {
            BString path = p->getModuleName(eip);
            const char* c = path.c_str();
            const char* slash = strrchr(c, '/');
            mod = slash ? slash + 1 : c;
            off = p->getModuleEip(eip);
        }
        auto key = std::make_tuple(pname, mod, off & ~0xFu);
        for (auto* m : {&win, &cum}) {
            ProfAgg& a = (*m)[key];
            a.ms += e.second.ms; a.instr += e.second.instr; a.entries += e.second.entries;
        }
        byModule[pname + ":" + mod] += e.second.ms;
        total += e.second.ms;
    }
    vnboxProfBlocks.clear();
    auto top = [](std::map<std::tuple<std::string, std::string, unsigned>, ProfAgg>& m, size_t n) {
        std::vector<std::pair<double, std::tuple<std::string, std::string, unsigned>>> v;
        for (auto& e : m) v.push_back({e.second.ms, e.first});
        std::sort(v.rbegin(), v.rend());
        if (v.size() > n) v.resize(n);
        return v;
    };
    klog_fmt("[prof] %.1fs jit ms/s=%.0f by module:", sec, total / sec);
    std::vector<std::pair<double, std::string>> mods;
    for (auto& e : byModule) mods.push_back({e.second, e.first});
    std::sort(mods.rbegin(), mods.rend());
    for (size_t i = 0; i < mods.size() && i < 8; i++) klog_fmt("[prof]   %5.1f%% %s", total ? 100 * mods[i].first / total : 0, mods[i].second.c_str());
    for (auto& t : top(win, 20)) {
        auto& a = win[t.second];
        klog_fmt("[prof]   %5.1f%% %s:%s+0x%x ms=%.0f instr=%llu entries=%llu", total ? 100 * t.first / total : 0, std::get<0>(t.second).c_str(),
            std::get<1>(t.second).c_str(), std::get<2>(t.second), a.ms, a.instr, a.entries);
    }
    // klog cuts lines at ~400 bytes: 4 items per line, join the [prof-cum] lines' items into one array
    std::string json;
    int n = 0;
    for (auto& t : top(cum, 400)) {
        auto& a = cum[t.second];
        char item[256];
        snprintf(item, sizeof(item), "%s[\"%s\",\"%s\",%u,%.1f,%llu,%llu]", json.empty() ? "" : ",", std::get<0>(t.second).c_str(),
            std::get<1>(t.second).c_str(), std::get<2>(t.second), a.ms, a.instr, a.entries);
        json += item;
        if (++n % 4 == 0) { klog_fmt("[prof-cum] %s", json.c_str()); json.clear(); }
    }
    if (!json.empty()) klog_fmt("[prof-cum] %s", json.c_str());
}

void mainloop() {
    static bool statsChecked = false;
    if (!statsChecked) {
        statsChecked = true;
        vnboxProfOn = EM_ASM_INT({
            var q = location.search;
            try { if (parent !== window) q += "&" + parent.location.search.slice(1); } catch (e) {}
            return /[?&]prof=1(&|$)/.test(q) ? 1 : 0;
        }) != 0;
        // on this page's URL or, for a player embedded in an iframe, the embedding page's
        vnboxStats.on = EM_ASM_INT({
            var q = location.search;
            try { if (parent !== window) q += "&" + parent.location.search.slice(1); } catch (e) {}
            return /[?&]stats=1(&|$)/.test(q) ? 1 : 0;
        }) != 0;
        if (vnboxStats.on) {
            char js[512];
            statsJsPart(js, sizeof(js), 1); // starts the JS side's counting
        }
    }
    if (vnboxStats.on) {
        statsTickStart();
    }
    if (vnboxProfOn) {
        profReport(emscripten_get_now());
    }
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
            statsLastMips = mipsRaw;
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
    if (vnboxStats.on) {
        if (idle) vnboxStats.ticksIdle++; else vnboxStats.ticksBusy++;
        statsLastTickEnd = emscripten_get_now();
    }
}

#endif

// A whole-number URL param of the player page, clamped to [lo, hi]; dflt when absent.
EM_JS(int, vnboxUrlInt, (const char* name, int dflt, int lo, int hi), {
    var v = parseInt(new URLSearchParams(location.search).get(UTF8ToString(name)), 10);
    return isNaN(v) ? dflt : Math.min(hi, Math.max(lo, v));
});

bool doMainLoop() {
    // Smoothness knobs, read before the guest runs. jitRunCount: interpreted runs before a block
    // is compiled; audioPeriod: frames per SDL audio callback (the non-worklet audio path).
#ifndef BOXEDWINE_MULTI_THREADED
    idleWaitMs = vnboxUrlInt("idleWait", idleWaitMs, 1, 1000);
#endif
    XServer::drawFps = vnboxUrlInt("fps", XServer::drawFps, 0, 240); // frames shown per second at most, 0 = unlocked
#if defined(BOXEDWINE_WASM_JIT) && !defined(__TEST)
    jitRunCount = vnboxUrlInt("jitRunCount", jitRunCount, 1, JIT_RUN_COUNT_MAX - 1);
#endif
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
    EM_ASM({
        var tick = WebAssembly.promising(wasmExports["boxedwine_tick"]);
        var channel = new MessageChannel();
        var lastFrame = performance.now();
        var frame = function() { lastFrame = performance.now(); requestAnimationFrame(frame); };
        var post = function() { channel.port2.postMessage(0); };
        var timer = 0;
        var wait = function(delay) { timer = setTimeout(function() { timer = 0; next(); }, delay); };
        var wake = function() { if (timer) { clearTimeout(timer); wait(0); } };
        "keydown keyup mousedown mouseup mousemove wheel touchstart touchend touchmove".split(" ").forEach(function(type) {
            window.addEventListener(type, wake, true);
        });
        var next = function() { tick().then(function(delay) {
            if (delay >= 0) wait(delay);
            else if (!document.hidden && performance.now() - lastFrame > 12) requestAnimationFrame(post);
            else post();
        }, function(e) { if (typeof vnboxReport === "function") vnboxReport("tick", e); else console.error("tick failed: " + e); }); };
        channel.port1.onmessage = next;
        requestAnimationFrame(frame);
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
extern "C" EMSCRIPTEN_KEEPALIVE int boxedwine_tick() {
    cdromRunQueuedInsert();
    mainloop();
    return jspiTickDelay;
}
#endif
#endif
