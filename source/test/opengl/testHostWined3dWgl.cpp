/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "boxedwine.h"

#if defined(__TEST) && defined(BOXEDWINE_HOST_WINED3D) && defined(BOXEDWINE_OPENGL)

// Host wined3d's window/DC table and GL entry points (source/wined3d/wglHost.cpp, wgl.c), without GL calls: a
// registered guest window keeps its HWND and gets one stable odd DC that maps back to it; memory DCs come and go;
// wglGetProcAddress / opengl32's GetProcAddress hand out the same pointer glcommon_* calls (pgl<name>), opengl32
// exports wglSwapBuffers but not the WGL extensions.
#include GLH
#include "../../opengl/glcommon.h"
#include "../../wined3d/hostwd3d_bridge.h"
#include "../../wined3d/hostwd3d_callbacks.h"
#include "testHostWined3dWgl.h"

void failed(const char* msg, ...);

namespace {

void OPENGL_CALL_TYPE sentinelEnable(GLenum cap) {}

void check(bool ok, const char* what) {
    if (!ok) {
        klog_fmt("host wined3d wgl test failed: %s", what);
        failed(what);
    }
}

} // namespace

void testHostWined3dWgl() {
    const U32 hwnd = 0x10020; // a Wine user handle: even
    hostWined3dRegisterWindow(hwnd, 77, 10, 20, 640, 480, 1, 0, 0, 640, 480);
    int32_t ox, oy;
    hostwd3d_client_offset(3, 29, 646, 512, 640, 480, &ox, &oy); // Decorated=Y: the X window is the client area
    check(ox == 0 && oy == 0, "client offset, decorated");
    hostwd3d_client_offset(3, 49, 646, 532, 640, 500, &ox, &oy); // + a 20 px menu bar, which is in the X window
    check(ox == 0 && oy == 20, "client offset, menu bar");
    hostwd3d_client_offset(3, 29, 646, 512, 646, 512, &ox, &oy); // undecorated: the X window is the window rect
    check(ox == 3 && oy == 29, "client offset, undecorated");
    hostwd3d_client_offset(0, 0, 640, 480, 640, 480, &ox, &oy); // frameless
    check(ox == 0 && oy == 0, "client offset, frameless");
    hostwd3d_window_info info;
    check(hostwd3d_window_get(hwnd, &info) && info.drawable == 77 && info.x == 10 && info.y == 20 && info.width == 640 && info.height == 480 && info.visible && !info.hostCreated, "registered window");

    U32 dc = hostwd3d_dc_for_window(hwnd);
    check(dc >= HOSTWD3D_HDC_BASE && (dc & 1), "window DC is odd, in the DC range");
    check(hostwd3d_dc_for_window(hwnd) == dc, "a window keeps its DC");
    check(hostwd3d_dc_window(dc) == hwnd, "WindowFromDC");
    check(!hostwd3d_dc_delete(dc), "a window DC isn't deleted by DeleteDC");

    check(hostwd3d_window_set_long(hwnd, -16, 0x10000000) == 0 && hostwd3d_window_set_long(hwnd, -16, 0x10cf0000) == 0x10000000, "GWL_STYLE");
    check(hostwd3d_window_set_long(hwnd, 5, 1) == 0, "other indexes are ignored");
    check(hostwd3d_window_set_pos(hwnd, -1, -1, 800, -1) && hostwd3d_window_get(hwnd, &info) && info.width == 800 && info.height == 480 && info.x == 10, "partial move/size");
    check(hostwd3d_window_set_visible(hwnd, 0) == 1 && hostwd3d_window_get(hwnd, &info) && !info.visible, "hide");

    U32 mem = hostwd3d_dc_create_memory();
    check(mem && mem != dc && (mem & 1) && !hostwd3d_dc_window(mem), "memory DC");
    check(hostwd3d_dc_delete(mem) && !hostwd3d_dc_delete(mem), "memory DC deleted once");
    check(!hostwd3d_dc_set_pixel_format(mem, 1), "no pixel format on a memory DC");

    hostWined3dUnregisterWindow(hwnd);
    check(!hostwd3d_window_get(hwnd, nullptr) && !hostwd3d_dc_window(dc), "unregistered window and its DC are gone");

    glEnable_func saved = pglEnable;
    pglEnable = sentinelEnable;
    check(hostwd3d_gl_proc("glEnable") == (void*)sentinelEnable, "gl proc is pglEnable");
    check(wd3d_opengl32_proc("glEnable") == (void*)sentinelEnable, "opengl32 GetProcAddress gives the same pointer");
    pglEnable = saved;
    check(wd3d_opengl32_proc("wglSwapBuffers") != nullptr && wd3d_opengl32_proc("wglGetPixelFormat") != nullptr, "opengl32 exports wglSwapBuffers/wglGetPixelFormat");
    check(wd3d_opengl32_proc("wglCreateContextAttribsARB") == nullptr, "opengl32 doesn't export WGL extensions");
    check(hostwd3d_gl_proc("wglCreateContext") == nullptr && hostwd3d_gl_proc(nullptr) == nullptr, "gl proc only answers gl names");
}

// Host wined3d sees glcommon's GL (wglHost.cpp routes, glcommonHost_* in glcommon.cpp): the fixed BoxedWine strings,
// the filtered extension list through glGetString/glGetStringi/GL_NUM_EXTENSIONS (here without a WebGL context, so
// the 10-name fallback list), the WebGL depth entry points; gl names outside Boxedwine's list are unavailable as for
// the guest.
void testHostWined3dGlRoutes() {
    typedef const GLubyte* (OPENGL_CALL_TYPE *FnGetString)(GLenum);
    typedef const GLubyte* (OPENGL_CALL_TYPE *FnGetStringi)(GLenum, GLuint);
    typedef void (OPENGL_CALL_TYPE *FnGetIntegerv)(GLenum, GLint*);
    FnGetString getString = (FnGetString)hostwd3d_gl_proc("glGetString");
    FnGetStringi getStringi = (FnGetStringi)hostwd3d_gl_proc("glGetStringi");
    FnGetIntegerv getIntegerv = (FnGetIntegerv)hostwd3d_gl_proc("glGetIntegerv");
    check(getString && getString == (FnGetString)hostwd3d_gl_route("glGetString") && (void*)getString != (void*)pglGetString, "glGetString is routed");
    check(wd3d_opengl32_proc("glGetString") == (void*)getString, "opengl32's glGetString is routed too");
#ifdef __EMSCRIPTEN__
    check(!strcmp((const char*)getString(GL_VERSION), "3.0 BoxedWine WebGL") && !strcmp((const char*)getString(GL_VENDOR), "BoxedWine")
        && !strcmp((const char*)getString(GL_RENDERER), "BoxedWine WebGL") && !strcmp((const char*)getString(0x8B8C), "3.00 BoxedWine WebGL"), "BoxedWine strings");
    GLint count = -1;
    getIntegerv(GL_NUM_EXTENSIONS, &count);
    check(count == 10, "GL_NUM_EXTENSIONS is the filtered count");
    check(!strcmp((const char*)getStringi(GL_EXTENSIONS, 0), "GL_ARB_fragment_shader") && !strcmp((const char*)getStringi(GL_EXTENSIONS, 9), "WGL_ARB_create_context_Invalid")
        && getStringi(GL_EXTENSIONS, 10) == nullptr, "glGetStringi walks the filtered list");
    check(strstr((const char*)getString(GL_EXTENSIONS), "GL_EXT_texture3D") != nullptr, "glGetString(GL_EXTENSIONS) is the filtered list");
    check(hostwd3d_gl_route("glDepthRange") && hostwd3d_gl_route("glClearDepth") && hostwd3d_gl_route("glDepthMask") && hostwd3d_gl_route("glDepthFunc"), "WebGL depth entry points");
#endif
    check(hostwd3d_gl_route("glTexSubImage2D") && hostwd3d_gl_route("glReadPixels") && hostwd3d_gl_route("glGetBufferSubData"), "pixel and buffer readback entry points");
    check(!hostwd3d_gl_route("glDrawArrays"), "plain functions aren't routed");
    check(hostwd3d_gl_proc("glNotAFunctionBoxedwineKnows") == nullptr, "names outside Boxedwine's GL list are unavailable");
}

// The parent calls wined3d makes are queued for the guest stub, oldest first (hostwd3d_callbacks.c).
void testHostWined3dCallbacks() {
    typedef void (*Destroyed)(void*);
    U32 alive = hostwd3d_parents_alive();
    hostwd3d_callback cb[4];
    while (hostwd3d_callbacks_drain(cb, 4)) {}

    void* p1 = hostwd3d_parent_wrap(0x1000, 0x2000);
    void* p2 = hostwd3d_parent_wrap(0x3000, 0); // the guest's null ops: nothing to call
    check(hostwd3d_parent_guest(p1) == 0x1000 && hostwd3d_parent_guest(nullptr) == 0 && hostwd3d_parents_alive() == alive + 2, "wrapped parents map back");
    Destroyed destroyed = *(Destroyed*)hostwd3d_parent_ops(); // wined3d_parent_ops.wined3d_object_destroyed
    destroyed(p2);
    destroyed(p1);
    destroyed(nullptr); // a sub-resource's parent
    check(hostwd3d_parents_alive() == alive && hostwd3d_callbacks_pending() == 1, "only parents with guest ops queue, all wrappers freed");

    // struct wined3d_device_parent { const ops* }; ops = { device_created, mode_changed, activate, sub_resource_created }
    void* dp = hostwd3d_device_parent_wrap(0x4000);
    void** ops = *(void***)dp;
    ((void (*)(void*, void*))ops[0])(dp, (void*)0x5000);
    ((void (*)(void*, int))ops[2])(dp, 1);
    void* parent = (void*)1;
    const void* parentOps = nullptr;
    int hr = ((int (*)(void*, int, void*, unsigned, void**, const void**))ops[3])(dp, 3, (void*)0x6000, 2, &parent, &parentOps);
    check(hr == 0 && parent == nullptr && parentOps == hostwd3d_parent_ops(), "sub-resource gets d3d11's answer");
    void* sp = hostwd3d_swapchain_state_parent_wrap(0x7000);
    ((void (*)(void*, int))(*(void***)sp)[0])(sp, 0);

    check(hostwd3d_callbacks_drain(cb, 2) == 2 && cb[0].kind == HOSTWD3D_CB_OBJECT_DESTROYED && cb[0].target == 0x2000 && cb[0].arg == 0x1000
        && cb[1].kind == HOSTWD3D_CB_DEVICE_CREATED && cb[1].target == 0x4000 && cb[1].value == 0x5000, "first two in order");
    check(hostwd3d_callbacks_drain(cb, 4) == 3 && cb[0].kind == HOSTWD3D_CB_ACTIVATE && cb[0].value == 1
        && cb[1].kind == HOSTWD3D_CB_SUB_RESOURCE_CREATED && cb[1].value == 0x6000 && cb[1].arg == 2
        && cb[2].kind == HOSTWD3D_CB_WINDOWED_STATE_CHANGED && cb[2].target == 0x7000 && cb[2].value == 0, "the rest in order");
    check(hostwd3d_callbacks_pending() == 0, "drained");
    for (U32 i = 0; i < 200; i++) { // ring grows past its first size without reordering
        destroyed(hostwd3d_parent_wrap(i, 0x9000));
    }
    hostwd3d_callback many[200];
    check(hostwd3d_callbacks_drain(many, 200) == 200 && many[0].arg == 0 && many[199].arg == 199, "200 queued in order");
    hostwd3d_device_parent_free(dp);
    hostwd3d_swapchain_state_parent_free(sp);
}

#endif
