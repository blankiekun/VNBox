#ifndef __VNBOX_IDLE_H__
#define __VNBOX_IDLE_H__

// Guest CPU caps for the web build's main loop (source/sdl/emscripten/mainloop.cpp, single-threaded only).
// Idle cap (idleCap=<percent>, default 20, 0 = off): once the screen has shown nothing but small changes
// for a while (a blinking cursor, an icon: under idleCapArea=<percent> of the screen, default 2) and
// nothing else happened (input, file reads, GL presents, audio running low), the guest waits after each
// frame it puts on screen so that it runs that share of the time, waiting at most idleCapHold=<ms> (50). Fixed cap (cpuCap=<percent>, default off): the guest runs at most that share of each 1/60 s.
// A cap only delays when the guest runs; guest clocks and timers keep real time.

enum VnboxWake { VNBOX_WAKE_INPUT, VNBOX_WAKE_MOTION, VNBOX_WAKE_FRAME, VNBOX_WAKE_FILE, VNBOX_WAKE_AUDIO, VNBOX_WAKE_GL };

#if defined(__EMSCRIPTEN__) && !defined(BOXEDWINE_MULTI_THREADED)
// something happened that needs full speed: lifts the idle cap now and for the reason's grace time
void vnboxIdleWake(VnboxWake reason);
// a shown software frame differed from the last one inside [x0,x1) x [y0,y1) of a width x height screen
void vnboxIdleFrameChanged(unsigned x0, unsigned y0, unsigned x1, unsigned y1, unsigned width, unsigned height);
// the guest put an image on a window: the end of one of its frames (the idle cap paces frames)
extern bool vnboxFramePut;
inline void vnboxIdleFramePut() { vnboxFramePut = true; }
#else
inline void vnboxIdleFramePut() {}
inline void vnboxIdleWake(VnboxWake) {}
inline void vnboxIdleFrameChanged(unsigned, unsigned, unsigned, unsigned, unsigned, unsigned) {}
#endif

#endif
