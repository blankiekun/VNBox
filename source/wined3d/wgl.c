/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

// Host wined3d: the opengl32 side wined3d links against (wgl*), compiled with Wine's headers and flags like
// wined3d itself. Contexts, pixel formats and GL entry points come from Boxedwine through hostwd3d_bridge.h.
// The WGL extensions offered are the ones the guest's winex11 offers on top of Boxedwine's GLX (create_context,
// create_context_profile, multisample, pixel_format, extensions_string, pixel_format_passthrough): no swap control,
// no query_renderer, so wined3d takes the same paths as the emulated build.
#include <stdarg.h>
#include <string.h>
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "wine/wgl.h"
#include "hostwd3d_bridge.h"

#define HDC_ID(dc) ((uint32_t)(ULONG_PTR)(dc))

static const char wglExtensions[] = "WGL_ARB_create_context WGL_ARB_create_context_profile WGL_ARB_extensions_string "
    "WGL_ARB_multisample WGL_ARB_pixel_format WGL_EXT_extensions_string WGL_WINE_pixel_format_passthrough";

HGLRC WINAPI wglCreateContext(HDC dc) {
    return (HGLRC)(ULONG_PTR)hostwd3d_gl_create_context(HDC_ID(dc), 0, 0, 0, 0, 0);
}

BOOL WINAPI wglDeleteContext(HGLRC ctx) {
    return hostwd3d_gl_delete_context((uint32_t)(ULONG_PTR)ctx);
}

HGLRC WINAPI wglGetCurrentContext(void) {
    return (HGLRC)(ULONG_PTR)hostwd3d_gl_current_context();
}

HDC WINAPI wglGetCurrentDC(void) {
    return (HDC)(ULONG_PTR)hostwd3d_gl_current_dc();
}

BOOL WINAPI wglMakeCurrent(HDC dc, HGLRC ctx) {
    return hostwd3d_gl_make_current(HDC_ID(dc), (uint32_t)(ULONG_PTR)ctx);
}

BOOL WINAPI wglShareLists(HGLRC org, HGLRC dst) {
    // shortcut: KOpenGL only shares at creation, and the web backend shares nothing anyway (one WebGL context per
    // GL context); wined3d only shares its own contexts, which it creates with wglCreateContextAttribsARB.
    hostwd3d_stub_once("wglShareLists");
    return TRUE;
}

static BOOL WINAPI host_wglSwapBuffers(HDC dc) {
    return hostwd3d_gl_swap(HDC_ID(dc));
}

static int WINAPI host_wglGetPixelFormat(HDC dc) {
    return hostwd3d_dc_get_pixel_format(HDC_ID(dc));
}

static HGLRC WINAPI host_wglCreateContextAttribsARB(HDC dc, HGLRC share, const int* attribs) {
    // WGL_CONTEXT_* values are the GLX_CONTEXT_* ones KOpenGL takes (profile and flag bits are identical).
    int major = 0, minor = 0, flags = 0, profile = 0;
    for (; attribs && attribs[0]; attribs += 2) {
        switch (attribs[0]) {
        case WGL_CONTEXT_MAJOR_VERSION_ARB: major = attribs[1]; break;
        case WGL_CONTEXT_MINOR_VERSION_ARB: minor = attribs[1]; break;
        case WGL_CONTEXT_FLAGS_ARB: flags = attribs[1]; break;
        case WGL_CONTEXT_PROFILE_MASK_ARB: profile = attribs[1]; break;
        default:
            hostwd3d_stub_once("wglCreateContextAttribsARB unknown attribute");
            return NULL;
        }
    }
    return (HGLRC)(ULONG_PTR)hostwd3d_gl_create_context(HDC_ID(dc), major, minor, profile, flags, (uint32_t)(ULONG_PTR)share);
}

static const char* WINAPI host_wglGetExtensionsStringARB(HDC dc) {
    return wglExtensions;
}

static const char* WINAPI host_wglGetExtensionsStringEXT(void) {
    return wglExtensions;
}

// One WGL_ARB_pixel_format attribute of a 1-based format, from its descriptor.
static BOOL pixelFormatAttrib(int format, int attrib, int* value) {
    PIXELFORMATDESCRIPTOR pfd;
    int sampleBuffers, samples, pbuffer;
    if (attrib == WGL_NUMBER_PIXEL_FORMATS_ARB) {
        *value = hostwd3d_pf_count();
        return TRUE;
    }
    if (!hostwd3d_pf_describe(format, &pfd) || !hostwd3d_pf_extra(format, &sampleBuffers, &samples, &pbuffer)) {
        return FALSE;
    }
    switch (attrib) {
    case WGL_DRAW_TO_WINDOW_ARB: *value = !!(pfd.dwFlags & PFD_DRAW_TO_WINDOW); break;
    case WGL_DRAW_TO_BITMAP_ARB: *value = !!(pfd.dwFlags & PFD_DRAW_TO_BITMAP); break;
    case WGL_DRAW_TO_PBUFFER_ARB: *value = pbuffer; break;
    case WGL_ACCELERATION_ARB:
        *value = (pfd.dwFlags & PFD_GENERIC_FORMAT) ? ((pfd.dwFlags & PFD_GENERIC_ACCELERATED) ? WGL_GENERIC_ACCELERATION_ARB : WGL_NO_ACCELERATION_ARB) : WGL_FULL_ACCELERATION_ARB;
        break;
    case WGL_NEED_PALETTE_ARB: *value = !!(pfd.dwFlags & PFD_NEED_PALETTE); break;
    case WGL_NEED_SYSTEM_PALETTE_ARB: *value = !!(pfd.dwFlags & PFD_NEED_SYSTEM_PALETTE); break;
    case WGL_SWAP_LAYER_BUFFERS_ARB: *value = !!(pfd.dwFlags & PFD_SWAP_LAYER_BUFFERS); break;
    case WGL_SWAP_METHOD_ARB:
        *value = (pfd.dwFlags & PFD_SWAP_EXCHANGE) ? WGL_SWAP_EXCHANGE_ARB : (pfd.dwFlags & PFD_SWAP_COPY) ? WGL_SWAP_COPY_ARB : WGL_SWAP_UNDEFINED_ARB;
        break;
    case WGL_NUMBER_OVERLAYS_ARB: case WGL_NUMBER_UNDERLAYS_ARB: case WGL_TRANSPARENT_ARB: *value = 0; break;
    case WGL_SUPPORT_GDI_ARB: *value = !!(pfd.dwFlags & PFD_SUPPORT_GDI); break;
    case WGL_SUPPORT_OPENGL_ARB: *value = !!(pfd.dwFlags & PFD_SUPPORT_OPENGL); break;
    case WGL_DOUBLE_BUFFER_ARB: *value = !!(pfd.dwFlags & PFD_DOUBLEBUFFER); break;
    case WGL_STEREO_ARB: *value = !!(pfd.dwFlags & PFD_STEREO); break;
    case WGL_PIXEL_TYPE_ARB: *value = pfd.iPixelType == PFD_TYPE_RGBA ? WGL_TYPE_RGBA_ARB : WGL_TYPE_COLORINDEX_ARB; break;
    case WGL_COLOR_BITS_ARB: *value = pfd.cColorBits; break;
    case WGL_RED_BITS_ARB: *value = pfd.cRedBits; break;
    case WGL_RED_SHIFT_ARB: *value = pfd.cRedShift; break;
    case WGL_GREEN_BITS_ARB: *value = pfd.cGreenBits; break;
    case WGL_GREEN_SHIFT_ARB: *value = pfd.cGreenShift; break;
    case WGL_BLUE_BITS_ARB: *value = pfd.cBlueBits; break;
    case WGL_BLUE_SHIFT_ARB: *value = pfd.cBlueShift; break;
    case WGL_ALPHA_BITS_ARB: *value = pfd.cAlphaBits; break;
    case WGL_ALPHA_SHIFT_ARB: *value = pfd.cAlphaShift; break;
    case WGL_ACCUM_BITS_ARB: *value = pfd.cAccumBits; break;
    case WGL_ACCUM_RED_BITS_ARB: *value = pfd.cAccumRedBits; break;
    case WGL_ACCUM_GREEN_BITS_ARB: *value = pfd.cAccumGreenBits; break;
    case WGL_ACCUM_BLUE_BITS_ARB: *value = pfd.cAccumBlueBits; break;
    case WGL_ACCUM_ALPHA_BITS_ARB: *value = pfd.cAccumAlphaBits; break;
    case WGL_DEPTH_BITS_ARB: *value = pfd.cDepthBits; break;
    case WGL_STENCIL_BITS_ARB: *value = pfd.cStencilBits; break;
    case WGL_AUX_BUFFERS_ARB: *value = pfd.cAuxBuffers; break;
    case WGL_SAMPLE_BUFFERS_ARB: *value = sampleBuffers; break;
    case WGL_SAMPLES_ARB: *value = samples; break;
    default: return FALSE;
    }
    return TRUE;
}

static BOOL WINAPI host_wglGetPixelFormatAttribivARB(HDC dc, int format, int layer, UINT count, const int* attribs, int* values) {
    if (layer) {
        return FALSE;
    }
    for (UINT i = 0; i < count; i++) {
        if (!pixelFormatAttrib(format, attribs[i], &values[i])) {
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL WINAPI host_wglGetPixelFormatAttribfvARB(HDC dc, int format, int layer, UINT count, const int* attribs, FLOAT* values) {
    if (layer) {
        return FALSE;
    }
    for (UINT i = 0; i < count; i++) {
        int value;
        if (!pixelFormatAttrib(format, attribs[i], &value)) {
            return FALSE;
        }
        values[i] = (FLOAT)value;
    }
    return TRUE;
}

static BOOL WINAPI host_wglChoosePixelFormatARB(HDC dc, const int* iattribs, const FLOAT* fattribs, UINT max, int* formats, UINT* count) {
    // shortcut: maps the attributes ChoosePixelFormat understands and returns its single best match; wined3d loads
    // this entry point but picks formats itself from wglGetPixelFormatAttribivARB.
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER_DONTCARE | PFD_DEPTH_DONTCARE, PFD_TYPE_RGBA};
    for (; iattribs && iattribs[0]; iattribs += 2) {
        int v = iattribs[1];
        switch (iattribs[0]) {
        case WGL_DRAW_TO_WINDOW_ARB: if (v) pfd.dwFlags |= PFD_DRAW_TO_WINDOW; break;
        case WGL_DOUBLE_BUFFER_ARB: pfd.dwFlags &= ~PFD_DOUBLEBUFFER_DONTCARE; if (v) pfd.dwFlags |= PFD_DOUBLEBUFFER; break;
        case WGL_PIXEL_TYPE_ARB: pfd.iPixelType = v == WGL_TYPE_COLORINDEX_ARB ? PFD_TYPE_COLORINDEX : PFD_TYPE_RGBA; break;
        case WGL_COLOR_BITS_ARB: pfd.cColorBits = (BYTE)v; break;
        case WGL_ALPHA_BITS_ARB: pfd.cAlphaBits = (BYTE)v; break;
        case WGL_DEPTH_BITS_ARB: pfd.dwFlags &= ~PFD_DEPTH_DONTCARE; pfd.cDepthBits = (BYTE)v; break;
        case WGL_STENCIL_BITS_ARB: pfd.cStencilBits = (BYTE)v; break;
        default: break;
        }
    }
    int best = hostwd3d_pf_choose(&pfd);
    *count = best && max ? 1 : 0;
    if (*count) {
        formats[0] = best;
    }
    return TRUE;
}

static BOOL WINAPI host_wglSetPixelFormatWINE(HDC dc, int format) {
    return hostwd3d_dc_set_pixel_format(HDC_ID(dc), format);
}

static PROC wglProc(const char* name) {
    static const struct { const char* name; PROC proc; } procs[] = {
        {"wglCreateContextAttribsARB", (PROC)host_wglCreateContextAttribsARB},
        {"wglGetExtensionsStringARB", (PROC)host_wglGetExtensionsStringARB},
        {"wglGetExtensionsStringEXT", (PROC)host_wglGetExtensionsStringEXT},
        {"wglGetPixelFormatAttribivARB", (PROC)host_wglGetPixelFormatAttribivARB},
        {"wglGetPixelFormatAttribfvARB", (PROC)host_wglGetPixelFormatAttribfvARB},
        {"wglChoosePixelFormatARB", (PROC)host_wglChoosePixelFormatARB},
        {"wglSetPixelFormatWINE", (PROC)host_wglSetPixelFormatWINE},
    };
    for (unsigned int i = 0; i < ARRAY_SIZE(procs); i++) {
        if (!strcmp(name, procs[i].name)) {
            return procs[i].proc;
        }
    }
    return NULL;
}

PROC WINAPI wglGetProcAddress(LPCSTR name) {
    if (!name) {
        return NULL;
    }
    if (!strncmp(name, "wgl", 3)) {
        return wglProc(name);
    }
    return (PROC)hostwd3d_gl_proc(name);
}

void* wd3d_opengl32_proc(const char* name) {
    static const struct { const char* name; PROC proc; } exports[] = {
        {"wglSwapBuffers", (PROC)host_wglSwapBuffers},
        {"wglGetPixelFormat", (PROC)host_wglGetPixelFormat},
        {"wglCreateContext", (PROC)wglCreateContext},
        {"wglDeleteContext", (PROC)wglDeleteContext},
        {"wglGetCurrentContext", (PROC)wglGetCurrentContext},
        {"wglGetCurrentDC", (PROC)wglGetCurrentDC},
        {"wglMakeCurrent", (PROC)wglMakeCurrent},
        {"wglShareLists", (PROC)wglShareLists},
        {"wglGetProcAddress", (PROC)wglGetProcAddress},
    };
    if (!name) {
        return NULL;
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(exports); i++) {
        if (!strcmp(name, exports[i].name)) {
            return (void*)exports[i].proc;
        }
    }
    // opengl32 exports only core GL, never the wgl extensions
    return strncmp(name, "wgl", 3) ? hostwd3d_gl_proc(name) : NULL;
}
