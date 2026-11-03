#ifndef __VNBOX_PARAM_H__
#define __VNBOX_PARAM_H__

// VNBox URL options (emscripten only; defined in source/sdl/emscripten/mainloop.cpp). Every option read in C++ goes
// through these two, with the option's name as a string literal: project/emscripten/vnbox-options.check.mjs checks
// the names against project/emscripten/vnbox-options.json, the list wrappers build their tuning menus from.
// The value comes from vnboxParam (lazyfs.js); where that is absent (node tests) from env var env, if given.

// The option as a whole number clamped to [lo, hi]; dflt when absent or not a number.
int vnboxUrlInt(const char* name, int dflt, int lo, int hi, const char* env = nullptr);
// True when the option is set to exactly value.
bool vnboxUrlIs(const char* name, const char* value, const char* env = nullptr);

// The guest cannot start: tells the page and a page embedding the player (vnbox-error, source "boot"). Nothing elsewhere.
#ifdef __EMSCRIPTEN__
void vnboxStartFailed(const char* why);
#else
inline void vnboxStartFailed(const char*) {}
#endif

#endif
