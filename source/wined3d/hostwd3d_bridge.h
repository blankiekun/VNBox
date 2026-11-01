/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

// Host wined3d (BOXEDWINE_HOST_WINED3D): the plain-C seam between the Win32 entry points wined3d links against
// (source/wined3d/{wgl,user32,gdi32}.c, compiled with Wine's headers and flags) and Boxedwine's host side (wglHost.cpp:
// KOpenGL, XServer, the host GL function pointers). Only fixed-width integers cross it, so neither side needs the
// other's headers.
//
// Handles. wined3d runs on the host and never sees guest Win32 objects; it only sees values that go through this
// table.
// - HWND: a window the guest stub registered (hostWined3dRegisterWindow) keeps the guest's HWND value, so the
//   window handles in wined3d's swapchain descs mean the same window on both sides. Wine user handles are always
//   even ((index << 1) + 0x20 | generation << 16), so windows wined3d creates itself (CreateWindowExA: the hidden
//   window it probes GL caps with) get odd values from HOSTWD3D_HOST_HWND_BASE and can't collide.
// - HDC: each window has one DC (class/own-DC semantics, what wined3d expects), created on first GetDC; memory DCs
//   (CreateCompatibleDC/CreateDCW) have no window. DC values are odd and start at HOSTWD3D_HDC_BASE.
// - HGLRC: the KOpenGL context id.
// - Pixel formats: 1-based index into PlatformOpenGL::iterateFormats order.
#ifndef HOSTWD3D_BRIDGE_H
#define HOSTWD3D_BRIDGE_H

#include <stdint.h>

#define HOSTWD3D_HOST_HWND_BASE 0x7f000001u
#define HOSTWD3D_HDC_BASE 0x7e000001u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hostwd3d_window_info {
    uint32_t drawable; // X window id (winex11's whole window for a guest window), 0 = none yet
    int32_t x, y;      // client area origin in screen coordinates
    int32_t width, height;
    uint32_t visible;
    uint32_t pixelFormat; // 1-based, 0 = not set
    uint32_t style, exStyle;
    uint32_t wndProc;     // what SetWindowLong(GWLP_WNDPROC) stored; never called on the host
    uint32_t hostCreated; // created by wined3d on the host (CreateWindowExA), not registered by the guest
} hostwd3d_window_info;

// Called by the guest stub (phase 2) before handing wined3d a window: guest HWND, X drawable, client rect on screen,
// and the window rect's size with the client's origin in it (frameLeft/Top), which place the GL child in the X window.
void hostWined3dRegisterWindow(uint32_t hwnd, uint32_t drawable, int32_t x, int32_t y, int32_t width, int32_t height, uint32_t visible,
        int32_t frameLeft, int32_t frameTop, int32_t windowWidth, int32_t windowHeight);
// Where the client area starts in winex11's X window of size xWidth x xHeight (see hostWined3dRegisterWindow).
void hostwd3d_client_offset(int32_t frameLeft, int32_t frameTop, int32_t windowWidth, int32_t windowHeight, int32_t xWidth, int32_t xHeight,
        int32_t* x, int32_t* y);
void hostWined3dUnregisterWindow(uint32_t hwnd);
// Around every forwarded call: makes host wined3d's GL context current again if guest GL changed the thread's, and
// gives the guest its own back after (enter returned 1: saved holds the guest's).
int hostwd3d_gl_enter(uint32_t saved[3]);
void hostwd3d_gl_leave(const uint32_t saved[3]);

// Windows wined3d creates on the host get a hidden X window of that size so a GL context can be made current on it.
uint32_t hostwd3d_window_create(int32_t width, int32_t height, uint32_t style, uint32_t exStyle);
int hostwd3d_window_destroy(uint32_t hwnd);
int hostwd3d_window_get(uint32_t hwnd, hostwd3d_window_info* info); // 0 = no such window
int hostwd3d_window_set_pos(uint32_t hwnd, int32_t x, int32_t y, int32_t width, int32_t height); // < 0 keeps the value
int hostwd3d_window_set_visible(uint32_t hwnd, uint32_t visible);
// GWL_STYLE / GWL_EXSTYLE / GWLP_WNDPROC; returns the previous value (0 for any other index)
uint32_t hostwd3d_window_set_long(uint32_t hwnd, int32_t index, uint32_t value);

uint32_t hostwd3d_dc_for_window(uint32_t hwnd); // the window's own DC, 0 = no such window
uint32_t hostwd3d_dc_create_memory(void);
int hostwd3d_dc_delete(uint32_t hdc);
uint32_t hostwd3d_dc_window(uint32_t hdc); // 0 for a memory DC or an unknown DC

void hostwd3d_screen(int32_t* width, int32_t* height, int32_t* bpp, int32_t* refresh);

// Pixel formats. The descriptor is PIXELFORMATDESCRIPTOR's 40-byte layout (Boxedwine's PixelFormat is the same).
int hostwd3d_pf_count(void);
int hostwd3d_pf_describe(int format, void* pfd40); // 1-based; 0 = no such format
int hostwd3d_pf_choose(const void* pfd40);         // best match, 0 = none
// What PIXELFORMATDESCRIPTOR lacks (wglGetPixelFormatAttribivARB maps the rest from the descriptor); 0 = no such format
int hostwd3d_pf_extra(int format, int* sampleBuffers, int* samples, int* pbuffer);
int hostwd3d_dc_set_pixel_format(uint32_t hdc, int format); // on the DC's window, like Win32
int hostwd3d_dc_get_pixel_format(uint32_t hdc);

// GL contexts through KOpenGL, on the calling guest thread (KThread::currentThread()).
uint32_t hostwd3d_gl_create_context(uint32_t hdc, int32_t major, int32_t minor, int32_t profile, int32_t flags, uint32_t share);
int hostwd3d_gl_delete_context(uint32_t ctx);
int hostwd3d_gl_make_current(uint32_t hdc, uint32_t ctx); // hdc 0 + ctx 0 releases
uint32_t hostwd3d_gl_current_context(void);
uint32_t hostwd3d_gl_current_dc(void);
int hostwd3d_gl_swap(uint32_t hdc);
// The host GL entry point Boxedwine's glcommon_* handlers call after marshaling (pgl<name> / ext_gl<name>), NULL if
// none. Only gl* names; wgl* is answered by wgl.c.
void* hostwd3d_gl_proc(const char* name);
void* hostwd3d_gl_route(const char* name); // the glcommon-equivalent replacement for name, or null

// opengl32.dll as wined3d sees it: it loads its core GL table and wglSwapBuffers/wglGetPixelFormat with
// GetProcAddress(GetModuleHandleA("opengl32.dll"), name); the kernel32 shim (module.c) routes GetProcAddress on its
// opengl32 handle here (wgl.c).
void* wd3d_opengl32_proc(const char* name);

// Logs a stubbed Win32 call once per name (klog), so phase 2 sees which stubs wined3d actually reaches.
void hostwd3d_stub_once(const char* name);

#ifdef __cplusplus
}
#endif

#endif
