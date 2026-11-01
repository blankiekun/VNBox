/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

// Host wined3d, Boxedwine side of hostwd3d_bridge.h: the window/DC table, pixel formats from PlatformOpenGL, GL
// contexts through KOpenGL (the same calls the guest's glX path makes in glcommon.cpp), and the host GL entry points.
#include "boxedwine.h"

#if defined(BOXEDWINE_HOST_WINED3D) && defined(BOXEDWINE_OPENGL)
#include GLH
#ifdef __EMSCRIPTEN__
#include <GLES2/gl2.h>
#include "../opengl/emscriptenGLProcAddress.h"
#endif
#include <SDL.h>
#include "knativesystem.h"
#include "kopengl.h"
#include "platformOpenGL.h"
#include "../opengl/glcommon.h"
#include "../x11/x11.h"
#include "hostwd3d_bridge.h"
#include <unordered_map>
#include <unordered_set>
#include <string>

namespace {

struct HostWindow {
    hostwd3d_window_info info = {};
    U32 dc = 0;
    U32 wholeWindow = 0; // guest window: winex11's X window; info.drawable is then our client child of it
};

struct HostDc {
    U32 hwnd = 0; // 0 = memory DC
};

struct HostState {
    BOXEDWINE_MUTEX mutex;
    std::unordered_map<U32, HostWindow> windows;
    std::unordered_map<U32, HostDc> dcs;
    std::unordered_map<U32, U32> currentDc; // KThread id -> DC its current context was made current on
    // Host wined3d's current context and its drawable. One for the process, like host wined3d's TLS (sync.c: one
    // thread; CSMT would make both per thread). KThread::currentContext and the WebGL binding are shared with the
    // guest's own GL (a guest wined3d for d3d9 in the same process, opengl32) and other guest threads:
    // hostwd3d_gl_enter puts host wined3d's back before each call, as wined3d assumes nobody changed them.
    U32 hostCtx = 0, hostDrawable = 0;
    U32 nextHostHwnd = HOSTWD3D_HOST_HWND_BASE;
    U32 nextDc = HOSTWD3D_HDC_BASE;
    std::vector<GLPixelFormatPtr> formats;
    std::unordered_set<std::string> stubsLogged;
};

HostState& state() {
    static HostState s;
    return s;
}

// Callers hold the mutex.
U32 newDc(HostState& s, U32 hwnd) {
    U32 dc = s.nextDc;
    s.nextDc += 2;
    s.dcs[dc].hwnd = hwnd;
    return dc;
}

GLPixelFormatPtr formatAt(int format) {
    HostState& s = state();
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(s.mutex);
    if (s.formats.empty()) {
        PlatformOpenGL::iterateFormats([&s](const GLPixelFormatPtr& f) { s.formats.push_back(f); });
    }
    if (format < 1 || format > (int)s.formats.size()) {
        return nullptr;
    }
    return s.formats[format - 1];
}

XDrawablePtr drawableForDc(U32 hdc) {
    HostState& s = state();
    U32 drawable = 0;
    {
        BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(s.mutex);
        auto dc = s.dcs.find(hdc);
        if (dc == s.dcs.end() || !dc->second.hwnd) {
            return nullptr;
        }
        auto w = s.windows.find(dc->second.hwnd);
        if (w == s.windows.end()) {
            return nullptr;
        }
        drawable = w->second.info.drawable;
    }
    return drawable ? XServer::getServer()->getDrawable(drawable) : nullptr;
}

} // namespace

extern "C" {

// A guest window: winex11 renders GL into a child X window over the client area (create_client_window); host
// wined3d gets the same, created here under winex11's whole window, so the GL canvas has the client size and input
// lands where winex11 expects it. Re-registering moves/resizes it.
// winex11's X window is the window's "visible rect": the window rect less the frame a window manager draws (all of it
// with Decorated=Y and a managed window, none without). Win32 coordinates can't place the client in it: -fitWindow moves
// the X window to 0,0 behind Wine's back (XWindow::fitToScreen). Sizes can: what the X window lacks of the window rect
// is the WM frame, split like a standard frame (left = right = bottom border, the rest on top: the caption).
void hostwd3d_client_offset(int32_t frameLeft, int32_t frameTop, int32_t windowWidth, int32_t windowHeight, int32_t xWidth, int32_t xHeight,
        int32_t* x, int32_t* y) {
    int32_t side = windowWidth > xWidth ? (windowWidth - xWidth) / 2 : 0;
    int32_t top = windowHeight > xHeight ? windowHeight - xHeight - side : 0;
    *x = frameLeft > side ? frameLeft - side : 0;
    *y = frameTop > top ? frameTop - top : 0;
}

void hostWined3dRegisterWindow(uint32_t hwnd, uint32_t drawable, int32_t x, int32_t y, int32_t width, int32_t height, uint32_t visible,
        int32_t frameLeft, int32_t frameTop, int32_t windowWidth, int32_t windowHeight) {
    XServer* server = XServer::getServer(true);
    XWindowPtr whole = (drawable && server) ? server->getWindow(drawable) : nullptr;
    U32 oldChild = 0, child = 0;
    {
        HostState& s = state();
        BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(s.mutex);
        HostWindow& w = s.windows[hwnd];
        if (w.wholeWindow && w.wholeWindow == drawable) {
            child = w.info.drawable;
        } else if (w.wholeWindow) {
            oldChild = w.info.drawable;
        }
    }
    if (oldChild) {
        server->destroyWindow(oldChild);
    }
    U32 cx = width > 0 ? (U32)width : 1, cy = height > 0 ? (U32)height : 1;
    S32 ox = x, oy = y;
    if (whole) {
        hostwd3d_client_offset(frameLeft, frameTop, windowWidth, windowHeight, (S32)whole->width(), (S32)whole->height(), &ox, &oy);
        XWindowPtr c = child ? server->getWindow(child) : nullptr;
        if (c) {
            c->moveResize(ox, oy, cx, cy);
        } else {
            c = server->createNewWindow(0, whole, cx, cy, whole->getDepth(), (U32)ox, (U32)oy, InputOutput, 0, server->visual);
            if (c) {
                c->mapWindow();
            }
        }
        child = c ? c->id : 0;
    } else {
        child = drawable; // not an X window (yet): used as it is
    }
    HostState& s = state();
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(s.mutex);
    HostWindow& w = s.windows[hwnd];
    hostwd3d_window_info& info = w.info;
    bool changed = info.drawable != child || info.width != width || info.height != height || info.x != x || info.y != y;
    w.wholeWindow = whole ? drawable : 0;
    info.drawable = child;
    info.x = x;
    info.y = y;
    info.width = width;
    info.height = height;
    info.visible = visible;
    if (changed) { // rare: creation, resize_buffers / resize_target with a new size
        klog_fmt("[wined3d] window %X registered: X window %X %ux%u, client %d,%d %dx%d at %d,%d in it, window %dx%d, GL child %X%s", hwnd, drawable,
                whole ? whole->width() : 0, whole ? whole->height() : 0, x, y, width, height, ox, oy, windowWidth, windowHeight, child, visible ? "" : " (hidden)");
    }
}

void hostWined3dUnregisterWindow(uint32_t hwnd) {
    U32 child = 0;
    {
        HostState& s = state();
        BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(s.mutex);
        auto w = s.windows.find(hwnd);
        if (w == s.windows.end()) {
            return;
        }
        if (w->second.dc) {
            s.dcs.erase(w->second.dc);
        }
        if (w->second.wholeWindow) {
            child = w->second.info.drawable;
        }
        s.windows.erase(w);
    }
    if (child && XServer::getServer(true)) {
        XServer::getServer()->destroyWindow(child);
    }
}

uint32_t hostwd3d_window_create(int32_t width, int32_t height, uint32_t style, uint32_t exStyle) {
    // A hidden (never mapped) X window, like winex11 gives a hidden Win32 window, so KOpenGL can bind a context to it.
    XServer* server = XServer::getServer();
    const XWindowPtr& root = server->getRoot();
    XWindowPtr wnd = server->createNewWindow(0, root, width > 0 ? width : 1, height > 0 ? height : 1, root->getDepth(), 0, 0, InputOutput, 0, server->visual);
    if (!wnd) {
        return 0;
    }
    HostState& s = state();
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(s.mutex);
    U32 hwnd = s.nextHostHwnd;
    s.nextHostHwnd += 2;
    hostwd3d_window_info& info = s.windows[hwnd].info;
    info.drawable = wnd->id;
    info.width = (S32)wnd->width();
    info.height = (S32)wnd->height();
    info.style = style;
    info.exStyle = exStyle;
    info.hostCreated = 1;
    return hwnd;
}

int hostwd3d_window_destroy(uint32_t hwnd) {
    HostState& s = state();
    U32 drawable = 0;
    {
        BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(s.mutex);
        auto w = s.windows.find(hwnd);
        if (w == s.windows.end()) {
            return 0;
        }
        if (w->second.info.hostCreated) {
            drawable = w->second.info.drawable;
        }
        if (w->second.dc) {
            s.dcs.erase(w->second.dc);
        }
        s.windows.erase(w);
    }
    if (drawable) {
        // Only windows the host created; a guest window's X window belongs to winex11.
        XServer::getServer()->destroyWindow(drawable);
    }
    return 1;
}

int hostwd3d_window_get(uint32_t hwnd, hostwd3d_window_info* info) {
    HostState& s = state();
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(s.mutex);
    auto w = s.windows.find(hwnd);
    if (w == s.windows.end()) {
        return 0;
    }
    if (info) {
        *info = w->second.info;
    }
    return 1;
}

int hostwd3d_window_set_pos(uint32_t hwnd, int32_t x, int32_t y, int32_t width, int32_t height) {
    HostState& s = state();
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(s.mutex);
    auto w = s.windows.find(hwnd);
    if (w == s.windows.end()) {
        return 0;
    }
    // shortcut: only the table changes; a guest window's real size/position follows the guest (phase 2 re-registers
    // it), a host-created window's X window keeps its size (it is only a context target).
    hostwd3d_window_info& info = w->second.info;
    if (x >= 0) info.x = x;
    if (y >= 0) info.y = y;
    if (width >= 0) info.width = width;
    if (height >= 0) info.height = height;
    return 1;
}

int hostwd3d_window_set_visible(uint32_t hwnd, uint32_t visible) {
    HostState& s = state();
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(s.mutex);
    auto w = s.windows.find(hwnd);
    if (w == s.windows.end()) {
        return 0;
    }
    U32 was = w->second.info.visible;
    w->second.info.visible = visible ? 1 : 0;
    return was ? 1 : 0;
}

uint32_t hostwd3d_window_set_long(uint32_t hwnd, int32_t index, uint32_t value) {
    HostState& s = state();
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(s.mutex);
    auto w = s.windows.find(hwnd);
    if (w == s.windows.end()) {
        return 0;
    }
    hostwd3d_window_info& info = w->second.info;
    U32* field = index == -16 ? &info.style : index == -20 ? &info.exStyle : index == -4 ? &info.wndProc : nullptr; // GWL_STYLE, GWL_EXSTYLE, GWLP_WNDPROC
    if (!field) {
        return 0;
    }
    U32 old = *field;
    *field = value;
    return old;
}

uint32_t hostwd3d_dc_for_window(uint32_t hwnd) {
    HostState& s = state();
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(s.mutex);
    auto w = s.windows.find(hwnd);
    if (w == s.windows.end()) {
        return 0;
    }
    if (!w->second.dc) {
        w->second.dc = newDc(s, hwnd);
    }
    return w->second.dc;
}

uint32_t hostwd3d_dc_create_memory(void) {
    HostState& s = state();
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(s.mutex);
    return newDc(s, 0);
}

int hostwd3d_dc_delete(uint32_t hdc) {
    HostState& s = state();
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(s.mutex);
    auto dc = s.dcs.find(hdc);
    if (dc == s.dcs.end() || dc->second.hwnd) {
        return 0; // a window's own DC lives as long as the window
    }
    s.dcs.erase(dc);
    return 1;
}

uint32_t hostwd3d_dc_window(uint32_t hdc) {
    HostState& s = state();
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(s.mutex);
    auto dc = s.dcs.find(hdc);
    return dc == s.dcs.end() ? 0 : dc->second.hwnd;
}

void hostwd3d_screen(int32_t* width, int32_t* height, int32_t* bpp, int32_t* refresh) {
    // Only answers from a running X server (the guest's winex11 starts it); none yet (tests): 800x600x32.
    XServer* server = XServer::getServer(true);
    XWindowPtr root = server ? server->getRoot() : nullptr;
    if (width) *width = root ? (S32)root->width() : 800;
    if (height) *height = root ? (S32)root->height() : 600;
    if (bpp) *bpp = root ? (S32)root->getDepth() : 32;
    if (refresh) *refresh = 60; // shortcut: the browser's real rate is unknown here; wined3d only reports it
}

int hostwd3d_pf_count(void) {
    formatAt(1); // fills the list
    HostState& s = state();
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(s.mutex);
    return (int)s.formats.size();
}

int hostwd3d_pf_describe(int format, void* pfd40) {
    static_assert(sizeof(PixelFormat) == 40, "PixelFormat must match PIXELFORMATDESCRIPTOR");
    GLPixelFormatPtr f = formatAt(format);
    if (!f) {
        return 0;
    }
    if (pfd40) {
        memcpy(pfd40, &f->pf, sizeof(PixelFormat));
    }
    return 1;
}

int hostwd3d_pf_extra(int format, int* sampleBuffers, int* samples, int* pbuffer) {
    GLPixelFormatPtr f = formatAt(format);
    if (!f) {
        return 0;
    }
    if (sampleBuffers) *sampleBuffers = f->sampleBuffers ? 1 : 0;
    if (samples) *samples = (int)f->samples;
    if (pbuffer) *pbuffer = f->pbuffer ? 1 : 0;
    return 1;
}

int hostwd3d_pf_choose(const void* pfd40) {
    // Same idea as the guest's glXChooseVisual (glcommon.cpp): OpenGL + RGBA + the requested buffering, then the
    // closest color/depth/stencil without going under the request.
    PixelFormat want;
    memcpy(&want, pfd40, sizeof(want));
    int count = hostwd3d_pf_count();
    int best = 0;
    S32 bestScore = 0;
    for (int i = 1; i <= count; i++) {
        GLPixelFormatPtr f = formatAt(i);
        const PixelFormat& pf = f->pf;
        if (!(pf.dwFlags & K_PFD_SUPPORT_OPENGL) || pf.iPixelType != want.iPixelType) continue;
        if ((want.dwFlags & K_PFD_DRAW_TO_WINDOW) && !(pf.dwFlags & K_PFD_DRAW_TO_WINDOW)) continue;
        if (!(want.dwFlags & K_PFD_DOUBLEBUFFER_DONTCARE) && ((want.dwFlags ^ pf.dwFlags) & K_PFD_DOUBLEBUFFER)) continue;
        if (pf.cColorBits < want.cColorBits || pf.cAlphaBits < want.cAlphaBits) continue;
        if (!(want.dwFlags & K_PFD_DEPTH_DONTCARE) && pf.cDepthBits < want.cDepthBits) continue;
        if (pf.cStencilBits < want.cStencilBits) continue;
        S32 score = 1000 - (pf.cColorBits - want.cColorBits) - (pf.cAlphaBits - want.cAlphaBits) - (pf.cStencilBits - want.cStencilBits) - (pf.cDepthBits - want.cDepthBits) / 2;
        if (!best || score > bestScore) {
            best = i;
            bestScore = score;
        }
    }
    return best;
}

int hostwd3d_dc_set_pixel_format(uint32_t hdc, int format) {
    if (!formatAt(format)) {
        return 0;
    }
    HostState& s = state();
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(s.mutex);
    auto dc = s.dcs.find(hdc);
    if (dc == s.dcs.end() || !dc->second.hwnd) {
        return 0;
    }
    auto w = s.windows.find(dc->second.hwnd);
    if (w == s.windows.end()) {
        return 0;
    }
    w->second.info.pixelFormat = (U32)format; // Win32 allows changing it here only through wglSetPixelFormatWINE
    return 1;
}

int hostwd3d_dc_get_pixel_format(uint32_t hdc) {
    HostState& s = state();
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(s.mutex);
    auto dc = s.dcs.find(hdc);
    if (dc == s.dcs.end() || !dc->second.hwnd) {
        return 0;
    }
    auto w = s.windows.find(dc->second.hwnd);
    return w == s.windows.end() ? 0 : (int)w->second.info.pixelFormat;
}

uint32_t hostwd3d_gl_create_context(uint32_t hdc, int32_t major, int32_t minor, int32_t profile, int32_t flags, uint32_t share) {
    KThread* thread = KThread::currentThread();
    GLPixelFormatPtr format = formatAt(hostwd3d_dc_get_pixel_format(hdc));
    if (!thread || !format) {
        return 0;
    }
    return KNativeSystem::getOpenGL()->glCreateContext(thread, format, major, minor, profile, flags, share);
}

int hostwd3d_gl_delete_context(uint32_t ctx) {
    KThread* thread = KThread::currentThread();
    if (!thread || !ctx) {
        return 0;
    }
    KNativeSystem::getOpenGL()->glDestroyContext(thread, ctx);
    HostState& s = state();
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(s.mutex);
    if (s.hostCtx == ctx) {
        s.hostCtx = s.hostDrawable = 0;
    }
    return 1;
}

int hostwd3d_gl_make_current(uint32_t hdc, uint32_t ctx) {
    // As gl_common_XMakeCurrent: KOpenGL binds, the thread remembers it for glRestoreCurrentContext.
    KThread* thread = KThread::currentThread();
    if (!thread) {
        return 0;
    }
    XDrawablePtr d = drawableForDc(hdc);
    if ((ctx && !d) || (!ctx && hdc)) {
        return 0;
    }
    if (!KNativeSystem::getOpenGL()->glMakeCurrent(thread, d, ctx)) {
        return 0;
    }
    thread->currentContext = ctx;
    thread->currentDrawable = d ? d->id : 0;
    thread->currentReadDrawable = thread->currentDrawable;
    HostState& s = state();
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(s.mutex);
    if (ctx) {
        s.currentDc[thread->id] = hdc;
    } else {
        s.currentDc.erase(thread->id);
    }
    s.hostCtx = ctx;
    s.hostDrawable = thread->currentDrawable;
    return 1;
}

int hostwd3d_gl_enter(uint32_t saved[3]) {
    KThread* thread = KThread::currentThread();
    U32 ctx, drawable;
    {
        HostState& s = state();
        BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(s.mutex);
        ctx = s.hostCtx;
        drawable = s.hostDrawable;
    }
    if (!thread || !ctx) {
        return 0;
    }
    if (thread->currentContext == ctx && thread->currentDrawable == drawable) {
        // usual case: only the WebGL binding may have moved (another guest thread's GL); cheap when it hasn't
        KNativeSystem::getOpenGL()->glRestoreCurrentContext(thread);
        return 0;
    }
    XDrawablePtr d = drawable ? XServer::getServer()->getDrawable(drawable) : nullptr;
    if (!d) {
        return 0;
    }
    saved[0] = thread->currentContext;
    saved[1] = thread->currentDrawable;
    saved[2] = thread->currentReadDrawable;
    KNativeSystem::getOpenGL()->glMakeCurrent(thread, d, ctx);
    thread->currentContext = ctx;
    thread->currentDrawable = drawable;
    thread->currentReadDrawable = drawable;
    return 1;
}

// Only the thread's record goes back: guest GL calls rebind their thread's context before each call (glcommon.cpp
// glRestoreCurrentContext), and unbinding here would detach host wined3d's drawable.
void hostwd3d_gl_leave(const uint32_t saved[3]) {
    KThread* thread = KThread::currentThread();
    if (thread) {
        thread->currentContext = saved[0];
        thread->currentDrawable = saved[1];
        thread->currentReadDrawable = saved[2];
    }
}

uint32_t hostwd3d_gl_current_context(void) {
    KThread* thread = KThread::currentThread();
    return thread ? thread->currentContext : 0;
}

uint32_t hostwd3d_gl_current_dc(void) {
    KThread* thread = KThread::currentThread();
    if (!thread || !thread->currentContext) {
        return 0;
    }
    HostState& s = state();
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(s.mutex);
    auto dc = s.currentDc.find(thread->id);
    return dc == s.currentDc.end() ? 0 : dc->second;
}

int hostwd3d_gl_swap(uint32_t hdc) {
    KThread* thread = KThread::currentThread();
    XDrawablePtr d = drawableForDc(hdc);
    if (!thread || !d) {
        return 0;
    }
    KNativeSystem::getOpenGL()->glSwapBuffers(thread, d);
    return 1;
}

// GL entry points host wined3d gets instead of the raw browser GL, so it sees what the guest's wined3d sees through
// glcommon_* (see hostwd3d_gl_routes below). Everything else is the raw function: glcommon_* only marshals those.
extern "C++" { // glcommon.cpp
const GLubyte* OPENGL_CALL_TYPE glcommonHost_glGetString(GLenum name);
const GLubyte* OPENGL_CALL_TYPE glcommonHost_glGetStringi(GLenum name, GLuint index);
void OPENGL_CALL_TYPE glcommonHost_glGetIntegerv(GLenum pname, GLint* data);
void OPENGL_CALL_TYPE glcommonHost_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid* pixels);
void OPENGL_CALL_TYPE glcommonHost_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid* pixels);
}

namespace {
#ifdef __EMSCRIPTEN__
// glcommon calls Emscripten's GLES entry points for these (the desktop proc-address ones don't update WebGL state).
void OPENGL_CALL_TYPE hostClearDepth(GLdouble depth) { glClearDepthf((GLfloat)depth); }
void OPENGL_CALL_TYPE hostDepthRange(GLdouble zNear, GLdouble zFar) { glDepthRangef((GLfloat)zNear, (GLfloat)zFar); }
void OPENGL_CALL_TYPE hostDepthFunc(GLenum func) { glDepthFunc(func); }
void OPENGL_CALL_TYPE hostDepthMask(GLboolean flag) { glDepthMask(flag); }
#endif
// WebGL's getBufferSubData has no C entry point: ext_glGetBufferSubData is Boxedwine's wrapper (glext.cpp), set once the
// first context is current, which can be after wined3d looks the name up.
void OPENGL_CALL_TYPE hostGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, void* data) {
    if (ext_glGetBufferSubData) ext_glGetBufferSubData(target, offset, size, data);
}

struct Route { const char* name; void* proc; };
const Route routes[] = {
    {"glGetString", (void*)glcommonHost_glGetString},
    {"glGetStringi", (void*)glcommonHost_glGetStringi},
    {"glGetIntegerv", (void*)glcommonHost_glGetIntegerv},
    {"glTexSubImage2D", (void*)glcommonHost_glTexSubImage2D},
    {"glReadPixels", (void*)glcommonHost_glReadPixels},
#ifdef __EMSCRIPTEN__
    {"glClearDepth", (void*)hostClearDepth},
    {"glDepthRange", (void*)hostDepthRange},
    {"glDepthFunc", (void*)hostDepthFunc},
    {"glDepthMask", (void*)hostDepthMask},
#endif
    {"glGetBufferSubData", (void*)hostGetBufferSubData},
    {"glGetBufferSubDataARB", (void*)hostGetBufferSubData},
};
} // namespace

void* hostwd3d_gl_route(const char* name) {
    for (const Route& r : routes) {
        if (name && !strcmp(name, r.name)) {
            return r.proc;
        }
    }
    return nullptr;
}

void* hostwd3d_gl_proc(const char* name) {
    if (void* routed = hostwd3d_gl_route(name)) {
        return routed;
    }
    // The pointers glcommon_* call: pgl<name> (core) and ext_gl<name> (extensions). They are filled when the first
    // context is made current (loadSdlExtensions), but wined3d resolves its core table before that, so a null
    // pointer falls back to the same SDL lookup that fills them.
    static std::unordered_map<std::string, void**>* table = nullptr;
    if (!table) {
        // Statements, not an initializer list: glfunctions.h has a stray ';' after one entry. It includes
        // glfunctions_ext.h itself.
        struct Proc { const char* name; void** ptr; };
        static Proc procs[4096];
        U32 count = 0;
#undef GL_FUNCTION
#define GL_FUNCTION(func, RET, PARAMS, ARGS, PRE, POST, LOG) if (count < 4096) procs[count++] = {"gl" #func, (void**)&pgl##func};
#undef GL_FUNCTION_FMT
#define GL_FUNCTION_FMT(func, RET, PARAMS, ARGS, PRE, POST, LOG) if (count < 4096) procs[count++] = {"gl" #func, (void**)&pgl##func};
#undef GL_FUNCTION_CUSTOM
#define GL_FUNCTION_CUSTOM(func, RET, PARAMS) if (count < 4096) procs[count++] = {"gl" #func, (void**)&pgl##func};
#undef GL_EXT_FUNCTION
#define GL_EXT_FUNCTION(func, RET, PARAMS) if (count < 4096) procs[count++] = {"gl" #func, (void**)&ext_gl##func};
#include "../opengl/glfunctions.h"
        if (count == 4096) {
            kwarn("host wined3d: GL function table full, raise its size");
        }
        table = new std::unordered_map<std::string, void**>();
        for (U32 i = 0; i < count; i++) {
            (*table)[procs[i].name] = procs[i].ptr;
        }
    }
    if (!name || strncmp(name, "gl", 2)) {
        return nullptr;
    }
    auto entry = table->find(name);
    if (entry == table->end()) {
        // As the guest's glXGetProcAddress (isOpenGLProcAddressAvailable): only the generated GL list, plus the two
        // core functions Boxedwine implements as callbacks.
        if (strcmp(name, "glFinish") && strcmp(name, "glFlush")) {
            return nullptr;
        }
    } else if (*entry->second) {
        return *entry->second;
    }
#ifdef __EMSCRIPTEN__
    if (boxedwineIsUnsupportedEmscriptenGLProcAddress(name)) {
        return nullptr;
    }
#endif
    return SDL_GL_GetProcAddress(name);
}

void hostwd3d_stub_once(const char* name) {
    HostState& s = state();
    {
        BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(s.mutex);
        if (!s.stubsLogged.insert(name).second) {
            return;
        }
    }
    klog_fmt("[vnbox] host wined3d: stubbed %s", name);
}

} // extern "C"

#endif
