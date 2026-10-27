#ifndef __VNBOX_STATS_H__
#define __VNBOX_STATS_H__

// stats=1 in the page URL (emscripten): cheap counters, logged and reset every ~2 s by the main loop.
struct VnboxStats {
    bool on = false;
    unsigned glCalls = 0, glGetErrors = 0, presentsGl = 0, draws = 0, timers = 0, ticksBusy = 0, ticksIdle = 0;
    unsigned tickWaits = 0, msgWaits = 0, inputs = 0, motions = 0;
    double glMs = 0, presentGlMs = 0, drawMs = 0, sliceMs = 0, timerLateMs = 0, timerLateMaxMs = 0;
    double tickLateMs = 0, tickLateMaxMs = 0, msgGapMs = 0, msgGapMaxMs = 0;
    // where guest slice time goes: syscalls (by number), JIT vs interpreter, wineserver, faults, root fs zip
    double sysMs = 0, wineserverMs = 0, zipMs = 0;
    unsigned long long instr = 0, sysPadInstr = 0, jitInstr = 0;
    unsigned jitEntries = 0, blocksCompiled = 0, wineserverRequests = 0, signals = 0, faults = 0, zipMisses = 0;
    unsigned sysCount[440] = {};
    double sysMsBy[440] = {};
};
extern VnboxStats vnboxStats;

#endif
