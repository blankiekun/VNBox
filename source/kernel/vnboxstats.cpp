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

// stats=1 / prof=1 (vnboxstats.h): the counters, what fills them from the scheduler, syscalls and GL calls,
// and (emscripten) the main loop's per-tick accounting and the [stats] / [prof] console lines.
#include "boxedwine.h"
#include "vnboxstats.h"
#include "vnboxwinereq.h"
#include <algorithm>
#include <map>
#include <tuple>
#include <string>
#include <string.h>
#include <vector>

VnboxStats vnboxStats;
bool vnboxProfOn = false;
unsigned vnboxProfTick = 0;
std::unordered_map<unsigned long long, VnboxProfEntry> vnboxProfBlocks;

void vnboxStatSliceRecord(KThread* thread, U64 diff, U64 sysCallTime) {
    vnboxStats.sliceMs += diff / 1000.0;
    vnboxStats.sysMs += sysCallTime / 1000.0;
    vnboxStats.instr += thread->cpu->blockInstructionCount;
    if (thread->process->name == "wineserver") vnboxStats.wineserverMs += diff / 1000.0;
    unsigned long long key = ((unsigned long long)thread->process->id << 24) | (thread->id & 0xFFFFFF);
    vnboxStats.threadMs[key] += diff / 1000.0;
    vnboxStats.threadRuns[key]++;
    if (!vnboxStats.processNames.count(thread->process->id)) vnboxStats.processNames[thread->process->id] = thread->process->name;
}

// stats=1: a Wine client sends each wineserver request as a 64-byte header with write(), or writev() when data follows.
static void statsClientRequest(CPU* cpu, U32 no, U32 buf, U32 len) {
    KThread* thread = cpu->thread;
    if (thread->process->name == "wineserver") return; // its replies
    if (no == 146) { // writev(fd, iov, count): the header is iov[0]
        if (!len) return;
        len = thread->memory->readd(buf + 4);
        buf = thread->memory->readd(buf);
    }
    if (len != 64) return;
    U32 req = thread->memory->readd(buf);
    if (req >= sizeof(vnboxStats.wineReq) / sizeof(vnboxStats.wineReq[0])) return;
    vnboxStats.wineReq[req]++;
    if (!vnboxStats.processNames.count(thread->process->id)) vnboxStats.processNames[thread->process->id] = thread->process->name;
    vnboxStats.wineReqByThread[((unsigned long long)thread->process->id << 40) | ((unsigned long long)(thread->id & 0xFFFFFF) << 16) | req]++;
}

void vnboxStatSyscallRecord(CPU* cpu, U32 statsNo, U32 statsArg2, U32 statsLen, U32 result, U64 diff, U32 padInstr) {
    vnboxStats.sysCount[statsNo]++;
    vnboxStats.sysMsBy[statsNo] += diff / 1000.0;
    vnboxStats.sysPadInstr += padInstr;
    // wineserver reads each request's fixed 64-byte header with one read()
    if (statsNo == 3 && statsLen == 64 && result == 64 && cpu->thread->process->name == "wineserver") vnboxStats.wineserverRequests++;
    if (result == (U32)(-K_WAIT)) vnboxStats.sysBlocked[statsNo]++;
    else if ((statsNo == 4 || statsNo == 146) && result < 0x80000000) statsClientRequest(cpu, statsNo, statsArg2, statsLen);
}

void vnboxStatGl(CPU* cpu, void (*fn)(CPU*), int kind) {
#ifdef __EMSCRIPTEN__
    double start = emscripten_get_now();
    fn(cpu);
    double ms = emscripten_get_now() - start;
    vnboxStats.glMs += ms;
    vnboxStats.glCalls++;
    if (kind == 1) vnboxStats.glGetErrors++;
    else if (kind == 2) { vnboxStats.glReadbacks++; vnboxStats.glReadbackMs += ms; vnboxStats.glReadbackKB += cpu->peek32(3) / 1024.0; } // ARG3: size
    else if (kind == 3) { vnboxStats.glReadPixels++; vnboxStats.glReadPixelsMs += ms; }
#else
    fn(cpu);
#endif
}

#ifdef __EMSCRIPTEN__
unsigned vnboxStatsMips = 0;

static double statsLastTickEnd = 0;
static double statsLastLog = 0;

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
static void statsTickStart(int asked) {
    double now = emscripten_get_now();
    if (statsLastTickEnd) {
        double gap = now - statsLastTickEnd;
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
            sec, vnboxStatsMips, s.sliceMs / sec / 10.0, s.ticksBusy, s.ticksIdle,
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

void vnboxStatsTick(int askedMs) {
    static bool statsChecked = false;
    if (!statsChecked) {
        statsChecked = true;
        vnboxProfOn = EM_ASM_INT({ return vnboxParam("prof") === "1" ? 1 : 0; }) != 0;
        vnboxStats.on = EM_ASM_INT({ return vnboxParam("stats") === "1" ? 1 : 0; }) != 0;
        if (vnboxStats.on) {
            char js[512];
            statsJsPart(js, sizeof(js), 1); // starts the JS side's counting
        }
    }
    if (vnboxStats.on) {
        statsTickStart(askedMs);
    }
    if (vnboxProfOn) {
        profReport(emscripten_get_now());
    }
}

void vnboxStatsTickEnd(bool idle) {
    if (vnboxStats.on) {
        if (idle) vnboxStats.ticksIdle++; else vnboxStats.ticksBusy++;
        statsLastTickEnd = emscripten_get_now();
    }
}
#endif
