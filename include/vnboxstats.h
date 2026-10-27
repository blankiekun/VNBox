#ifndef __VNBOX_STATS_H__
#define __VNBOX_STATS_H__

#include <unordered_map>

// stats=1 in the page URL (emscripten): cheap counters, logged and reset every ~2 s by the main loop.
struct VnboxStats {
    bool on = false;
    unsigned glCalls = 0, glGetErrors = 0, presentsGl = 0, draws = 0, timers = 0, ticksBusy = 0, ticksIdle = 0;
    unsigned tickWaits = 0, msgWaits = 0, inputs = 0, motions = 0;
    unsigned presentsSw = 0, skippedSame = 0, busyDropped = 0; // software presents: shown, unchanged (skipped), JS side busy
    double glMs = 0, presentGlMs = 0, drawMs = 0, sliceMs = 0, timerLateMs = 0, timerLateMaxMs = 0;
    double tickLateMs = 0, tickLateMaxMs = 0, msgGapMs = 0, msgGapMaxMs = 0;
    // where guest slice time goes: syscalls (by number), JIT vs interpreter, wineserver, faults, root fs zip
    double sysMs = 0, wineserverMs = 0, zipMs = 0;
    unsigned long long instr = 0, sysPadInstr = 0, jitInstr = 0;
    unsigned jitEntries = 0, blocksCompiled = 0, wineserverRequests = 0, signals = 0, faults = 0, zipMisses = 0;
    unsigned audioUnderruns = 0; double audioQueuedMinMs = -1; // SDL audio path: callbacks that ran dry, least queued at a callback
    unsigned sysCount[440] = {};
    double sysMsBy[440] = {};
    unsigned sysBlocked[440] = {}; // calls that blocked (K_WAIT); a woken thread runs the call again, so sysCount includes the retry
    // wineserver requests the clients send (64-byte header write), by request number and by sending thread
    unsigned wineReq[320] = {};
    std::unordered_map<unsigned long long, unsigned> wineReqByThread; // (process id << 40 | thread id << 16 | request) -> count
    std::unordered_map<unsigned, BString> processNames; // process id -> name, for the log line
    // guest time by thread: (process id << 24 | thread id) -> ms in slices, and how many slices it ran
    std::unordered_map<unsigned long long, double> threadMs;
    std::unordered_map<unsigned long long, unsigned> threadRuns;
};
extern VnboxStats vnboxStats;

#endif
