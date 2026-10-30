#ifndef __VNBOX_STATS_H__
#define __VNBOX_STATS_H__

#include <unordered_map>

// stats=1 in the page URL (emscripten): cheap counters, logged and reset every ~2 s by the main loop.
struct VnboxStats {
    bool on = false;
    unsigned glCalls = 0, glGetErrors = 0, presentsGl = 0, draws = 0, timers = 0, ticksBusy = 0, ticksIdle = 0;
    unsigned tickWaits = 0, msgWaits = 0, inputs = 0, motions = 0;
    unsigned presentsSw = 0, skippedSame = 0, busyDropped = 0; // software presents: shown, unchanged (skipped), JS side busy
    // XServer::draw calls past the fps deadline (they composite every window, then present) and their ms; software present ms within them
    unsigned composites = 0; double compositeMs = 0, presentSwMs = 0;
    // JIT code pages: CodePage write calls (the slow path), code removed because of a write, ops decoded NO_JIT as dynamic code
    unsigned codePageWrites = 0, codeRemovedByWrite = 0, opsNoJitDynamic = 0;
    // GPU->CPU reads (each one waits for the GPU): getBufferSubData, readPixels/getTexImage
    unsigned glReadbacks = 0, glReadPixels = 0; double glReadbackMs = 0, glReadPixelsMs = 0, glReadbackKB = 0;
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

// prof=1 in the page URL (emscripten): where guest JIT time goes. One compiled block entry in VNBOX_PROF_EVERY is timed
// until the next entry (minus one clock read and any syscalls) and charged, scaled up, to that block (key: process id << 32 | block eip); the main loop prints
// the top entries. Blocks don't tail-call each other while it is on, so every entry is seen.
struct VnboxProfEntry {
    double ms = 0;
    unsigned long long instr = 0, entries = 0;
};
extern bool vnboxProfOn;
extern unsigned vnboxProfTick;
#define VNBOX_PROF_EVERY 32
extern std::unordered_map<unsigned long long, VnboxProfEntry> vnboxProfBlocks;

// The counting itself (vnboxstats.cpp), one line at each call site; each does nothing while stats=1 is off.
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
class CPU;
class KThread;
void vnboxStatSliceRecord(KThread* thread, U64 diffUs, U64 sysUs);
void vnboxStatSyscallRecord(CPU* cpu, U32 no, U32 arg2, U32 len, U32 result, U64 diffUs, U32 padInstr);
void vnboxStatGl(CPU* cpu, void (*fn)(CPU*), int kind); // runs fn timed; kind 1 GetError, 2 buffer readback, 3 pixel readback

// clock for vnboxStatDraw, 0 while off
inline double vnboxStatNow() {
#ifdef __EMSCRIPTEN__
    return vnboxStats.on ? emscripten_get_now() : 0;
#else
    return 0;
#endif
}
inline void vnboxStatDraw(double start) {
#ifdef __EMSCRIPTEN__
    if (vnboxStats.on) { vnboxStats.draws++; vnboxStats.drawMs += emscripten_get_now() - start; }
#endif
}
inline void vnboxStatTimer(double lateMs) {
    if (!vnboxStats.on) return;
    vnboxStats.timers++;
    vnboxStats.timerLateMs += lateMs;
    if (lateMs > vnboxStats.timerLateMaxMs) vnboxStats.timerLateMaxMs = lateMs;
}
inline void vnboxStatSlice(KThread* thread, U64 diffUs, U64 sysUs) { if (vnboxStats.on) vnboxStatSliceRecord(thread, diffUs, sysUs); }
// EAX is guest-chosen; the arrays cover the known syscalls
inline void vnboxStatSyscall(CPU* cpu, U32 no, U32 arg2, U32 len, U32 result, U64 diffUs, U32 padInstr) {
    if (vnboxStats.on && no < 440) vnboxStatSyscallRecord(cpu, no, arg2, len, result, diffUs, padInstr);
}
// one run of chained JIT blocks: ranInstr counted since the first block, padStart = sysPadInstr then
inline void vnboxStatJit(U32 ranInstr, unsigned long long padStart) {
    if (vnboxStats.on) { vnboxStats.jitEntries++; vnboxStats.jitInstr += ranInstr - (U32)(vnboxStats.sysPadInstr - padStart); }
}

#ifdef __EMSCRIPTEN__
// The main loop's side (mainloop.cpp): reads stats=1/prof=1 on the first tick, then per tick.
extern unsigned vnboxStatsMips; // the last MIPS sample, for the [stats] line
void vnboxStatsTick(int askedMs); // tick start; askedMs: the timeout the tick waited for, -1 a message
void vnboxStatsTickEnd(bool idle);
#endif

#endif
