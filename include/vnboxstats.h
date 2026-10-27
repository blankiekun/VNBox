#ifndef __VNBOX_STATS_H__
#define __VNBOX_STATS_H__

// stats=1 in the page URL (emscripten): cheap counters, logged and reset every ~2 s by the main loop.
struct VnboxStats {
    bool on = false;
    unsigned glCalls = 0, glGetErrors = 0, presentsGl = 0, draws = 0, timers = 0, ticksBusy = 0, ticksIdle = 0;
    unsigned tickWaits = 0, msgWaits = 0, inputs = 0, motions = 0;
    double glMs = 0, presentGlMs = 0, drawMs = 0, sliceMs = 0, timerLateMs = 0, timerLateMaxMs = 0;
    double tickLateMs = 0, tickLateMaxMs = 0, msgGapMs = 0, msgGapMaxMs = 0;
};
extern VnboxStats vnboxStats;

#endif
