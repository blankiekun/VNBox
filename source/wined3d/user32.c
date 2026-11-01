/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

// Host wined3d: the user32 calls wined3d makes, over the window table in hostwd3d_bridge.h. There are no real
// Win32 windows on the host: a guest window is what the guest stub registered (its HWND, X drawable, client rect),
// a window wined3d creates itself (its hidden GL-caps probe window) is a hidden X window. No messages, no window
// procedures, no hooks, no timers: those are recorded or refused, and each stub logs once (hostwd3d_stub_once).
// One monitor: the X root window.
#include <stdarg.h>
#include <string.h>
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "hostwd3d_bridge.h"

#define HWND_ID(w) ((uint32_t)(ULONG_PTR)(w))
#define DESKTOP_HWND 0x7ffffff1u // odd, so never a guest HWND or a host-created one
#define PRIMARY_MONITOR ((HMONITOR)(ULONG_PTR)0x7ffffff3u)
#define FAKE_HOOK ((HHOOK)(ULONG_PTR)0x7ffffff5u)

static const WCHAR displayName[] = {'\\','\\','.','\\','D','I','S','P','L','A','Y','1',0};

// Local rect helpers: SetRect & co. are user32 exports wined3d doesn't import, so this file doesn't either.
static void setRect(RECT* rect, LONG left, LONG top, LONG right, LONG bottom) {
    rect->left = left;
    rect->top = top;
    rect->right = right;
    rect->bottom = bottom;
}

static BOOL rectEmpty(const RECT* rect) {
    return rect->left >= rect->right || rect->top >= rect->bottom;
}

static void screenRect(RECT* rect) {
    int32_t w, h;
    hostwd3d_screen(&w, &h, NULL, NULL);
    setRect(rect, 0, 0, w, h);
}

// The desktop window exists from the first call that needs it, sized like the screen, with no drawable.
static uint32_t desktop(void) {
    static uint32_t registered;
    if (!registered) {
        int32_t w, h;
        hostwd3d_screen(&w, &h, NULL, NULL);
        hostWined3dRegisterWindow(DESKTOP_HWND, 0, 0, 0, w, h, 1, 0, 0, w, h);
        registered = DESKTOP_HWND;
    }
    return registered;
}

static BOOL windowInfo(HWND hwnd, hostwd3d_window_info* info) {
    return hostwd3d_window_get(hwnd ? HWND_ID(hwnd) : desktop(), info);
}

BOOL WINAPI AdjustWindowRectEx(LPRECT rect, DWORD style, BOOL menu, DWORD exStyle) {
    // shortcut: no frames on the host, so window and client rects are the same; a guest window's real frame is the
    // guest's business (phase 2 registers its client rect).
    return TRUE;
}

LRESULT WINAPI CallNextHookEx(HHOOK hook, INT code, WPARAM wp, LPARAM lp) {
    return 0;
}

LRESULT WINAPI CallWindowProcA(WNDPROC proc, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    hostwd3d_stub_once("CallWindowProcA");
    return 0;
}

LRESULT WINAPI CallWindowProcW(WNDPROC proc, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    hostwd3d_stub_once("CallWindowProcW");
    return 0;
}

LONG WINAPI ChangeDisplaySettingsExW(LPCWSTR device, LPDEVMODEW mode, HWND hwnd, DWORD flags, LPVOID param) {
    // The browser decides the screen: the current mode (or a reset to it) succeeds, anything else is refused.
    int32_t w, h, bpp;
    hostwd3d_screen(&w, &h, &bpp, NULL);
    if (!mode) {
        return DISP_CHANGE_SUCCESSFUL;
    }
    if ((!(mode->dmFields & DM_PELSWIDTH) || (int32_t)mode->dmPelsWidth == w) &&
        (!(mode->dmFields & DM_PELSHEIGHT) || (int32_t)mode->dmPelsHeight == h)) {
        return DISP_CHANGE_SUCCESSFUL;
    }
    hostwd3d_stub_once("ChangeDisplaySettingsExW to another mode");
    return DISP_CHANGE_BADMODE;
}

BOOL WINAPI ClientToScreen(HWND hwnd, LPPOINT pt) {
    hostwd3d_window_info info;
    if (!windowInfo(hwnd, &info)) {
        return FALSE;
    }
    pt->x += info.x;
    pt->y += info.y;
    return TRUE;
}

BOOL WINAPI ScreenToClient(HWND hwnd, LPPOINT pt) {
    hostwd3d_window_info info;
    if (!windowInfo(hwnd, &info)) {
        return FALSE;
    }
    pt->x -= info.x;
    pt->y -= info.y;
    return TRUE;
}

INT WINAPI MapWindowPoints(HWND from, HWND to, LPPOINT pts, UINT count) {
    hostwd3d_window_info a, b;
    if (!windowInfo(from, &a) || !windowInfo(to, &b)) {
        return 0;
    }
    LONG dx = a.x - b.x, dy = a.y - b.y;
    for (UINT i = 0; i < count; i++) {
        pts[i].x += dx;
        pts[i].y += dy;
    }
    return MAKELONG(LOWORD(dx), LOWORD(dy));
}

HICON WINAPI CreateIconIndirect(PICONINFO info) {
    hostwd3d_stub_once("CreateIconIndirect"); // d3d cursor images: phase 3
    return NULL;
}

HWND WINAPI CreateWindowExA(DWORD exStyle, LPCSTR className, LPCSTR name, DWORD style, INT x, INT y, INT width, INT height, HWND parent, HMENU menu, HINSTANCE instance, LPVOID param) {
    uint32_t hwnd = hostwd3d_window_create(width, height, style, exStyle);
    if (hwnd && (style & WS_VISIBLE)) {
        hostwd3d_window_set_visible(hwnd, 1);
    }
    return (HWND)(ULONG_PTR)hwnd;
}

BOOL WINAPI DestroyWindow(HWND hwnd) {
    return hostwd3d_window_destroy(HWND_ID(hwnd));
}

LRESULT WINAPI DefWindowProcA(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return 0;
}

LRESULT WINAPI DefWindowProcW(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return 0;
}

BOOL WINAPI DestroyCursor(HCURSOR cursor) {
    return TRUE;
}

BOOL WINAPI EnumDisplayDevicesW(LPCWSTR device, DWORD index, LPDISPLAY_DEVICEW dd, DWORD flags) {
    static const WCHAR adapterString[] = {'B','o','x','e','d','w','i','n','e',0};
    static const WCHAR monitorName[] = {'\\','\\','.','\\','D','I','S','P','L','A','Y','1','\\','M','o','n','i','t','o','r','0',0};
    if (index || !dd) {
        return FALSE;
    }
    memset((char*)dd + sizeof(dd->cb), 0, dd->cb - sizeof(dd->cb));
    if (!device) {
        memcpy(dd->DeviceName, displayName, sizeof(displayName));
        memcpy(dd->DeviceString, adapterString, sizeof(adapterString));
        dd->StateFlags = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE;
    } else {
        memcpy(dd->DeviceName, monitorName, sizeof(monitorName));
        memcpy(dd->DeviceString, adapterString, sizeof(adapterString));
        dd->StateFlags = DISPLAY_DEVICE_ATTACHED | DISPLAY_DEVICE_ACTIVE;
    }
    return TRUE;
}

BOOL WINAPI EnumDisplayMonitors(HDC dc, LPRECT clip, MONITORENUMPROC proc, LPARAM lp) {
    RECT rect;
    screenRect(&rect);
    return proc(PRIMARY_MONITOR, NULL, &rect, lp);
}

BOOL WINAPI EnumDisplaySettingsExW(LPCWSTR device, DWORD index, LPDEVMODEW mode, DWORD flags) {
    // Only the current mode exists (ENUM_CURRENT_SETTINGS, ENUM_REGISTRY_SETTINGS or index 0).
    int32_t w, h, bpp, refresh;
    if (index != ENUM_CURRENT_SETTINGS && index != ENUM_REGISTRY_SETTINGS && index) {
        return FALSE;
    }
    hostwd3d_screen(&w, &h, &bpp, &refresh);
    WORD size = mode->dmSize, extra = mode->dmDriverExtra;
    memset(mode, 0, size);
    mode->dmSize = size;
    mode->dmDriverExtra = extra;
    memcpy(mode->dmDeviceName, displayName, sizeof(displayName));
    mode->dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY | DM_POSITION | DM_DISPLAYORIENTATION | DM_DISPLAYFLAGS;
    mode->dmPelsWidth = w;
    mode->dmPelsHeight = h;
    mode->dmBitsPerPel = bpp;
    mode->dmDisplayFrequency = refresh;
    return TRUE;
}

BOOL WINAPI EnumDisplaySettingsW(LPCWSTR device, DWORD index, LPDEVMODEW mode) {
    return EnumDisplaySettingsExW(device, index, mode, 0);
}

BOOL WINAPI GetClientRect(HWND hwnd, LPRECT rect) {
    hostwd3d_window_info info;
    if (!windowInfo(hwnd, &info)) {
        return FALSE;
    }
    setRect(rect, 0, 0, info.width, info.height);
    return TRUE;
}

BOOL WINAPI GetWindowRect(HWND hwnd, LPRECT rect) {
    hostwd3d_window_info info;
    if (!windowInfo(hwnd, &info)) {
        return FALSE;
    }
    setRect(rect, info.x, info.y, info.x + info.width, info.y + info.height);
    return TRUE;
}

BOOL WINAPI GetCursorPos(LPPOINT pt) {
    hostwd3d_stub_once("GetCursorPos"); // d3d9 cursor emulation: phase 3
    pt->x = pt->y = 0;
    return TRUE;
}

BOOL WINAPI SetCursorPos(INT x, INT y) {
    hostwd3d_stub_once("SetCursorPos");
    return TRUE;
}

HCURSOR WINAPI SetCursor(HCURSOR cursor) {
    hostwd3d_stub_once("SetCursor");
    return NULL;
}

HDC WINAPI GetDC(HWND hwnd) {
    return (HDC)(ULONG_PTR)hostwd3d_dc_for_window(hwnd ? HWND_ID(hwnd) : desktop());
}

HDC WINAPI GetDCEx(HWND hwnd, HRGN clip, DWORD flags) {
    return GetDC(hwnd);
}

INT WINAPI ReleaseDC(HWND hwnd, HDC dc) {
    return 1; // a window's DC is its own DC, kept until the window goes
}

HWND WINAPI GetDesktopWindow(void) {
    return (HWND)(ULONG_PTR)desktop();
}

HWND WINAPI WindowFromDC(HDC dc) {
    return (HWND)(ULONG_PTR)hostwd3d_dc_window((uint32_t)(ULONG_PTR)dc);
}

BOOL WINAPI GetMonitorInfoW(HMONITOR monitor, LPMONITORINFO info) {
    if (monitor != PRIMARY_MONITOR || info->cbSize < sizeof(MONITORINFO)) {
        return FALSE;
    }
    screenRect(&info->rcMonitor);
    info->rcWork = info->rcMonitor;
    info->dwFlags = MONITORINFOF_PRIMARY;
    if (info->cbSize >= sizeof(MONITORINFOEXW)) {
        memcpy(((MONITORINFOEXW*)info)->szDevice, displayName, sizeof(displayName));
    }
    return TRUE;
}

HMONITOR WINAPI MonitorFromWindow(HWND hwnd, DWORD flags) {
    return PRIMARY_MONITOR;
}

static LONG getLong(HWND hwnd, INT index) {
    hostwd3d_window_info info;
    if (!windowInfo(hwnd, &info)) {
        return 0;
    }
    switch (index) {
    case GWL_STYLE: return (LONG)info.style;
    case GWL_EXSTYLE: return (LONG)info.exStyle;
    case GWLP_WNDPROC: return (LONG)info.wndProc;
    default: return 0;
    }
}

LONG WINAPI GetWindowLongA(HWND hwnd, INT index) {
    return getLong(hwnd, index);
}

LONG WINAPI GetWindowLongW(HWND hwnd, INT index) {
    return getLong(hwnd, index);
}

static LONG setLong(HWND hwnd, INT index, LONG value) {
    if (index == GWLP_WNDPROC) {
        // wined3d subclasses the device window to watch focus/activation; nothing calls the stored proc here.
        hostwd3d_stub_once("SetWindowLong(GWLP_WNDPROC)");
    }
    return (LONG)hostwd3d_window_set_long(HWND_ID(hwnd), index, (uint32_t)value);
}

LONG WINAPI SetWindowLongA(HWND hwnd, INT index, LONG value) {
    return setLong(hwnd, index, value);
}

LONG WINAPI SetWindowLongW(HWND hwnd, INT index, LONG value) {
    return setLong(hwnd, index, value);
}

DWORD WINAPI GetWindowThreadProcessId(HWND hwnd, LPDWORD pid) {
    // Every window counts as the caller's: wined3d then changes window state inline instead of on a new thread
    // (swapchain.c set_window_state), and the host has one thread per guest thread anyway.
    if (pid) {
        *pid = GetCurrentProcessId();
    }
    return GetCurrentThreadId();
}

BOOL WINAPI IntersectRect(LPRECT dst, const RECT* a, const RECT* b) {
    if (rectEmpty(a) || rectEmpty(b) || a->left >= b->right || b->left >= a->right || a->top >= b->bottom || b->top >= a->bottom) {
        setRect(dst, 0, 0, 0, 0);
        return FALSE;
    }
    dst->left = a->left > b->left ? a->left : b->left;
    dst->right = a->right < b->right ? a->right : b->right;
    dst->top = a->top > b->top ? a->top : b->top;
    dst->bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
    return TRUE;
}

BOOL WINAPI IsWindow(HWND hwnd) {
    return hostwd3d_window_get(HWND_ID(hwnd), NULL);
}

BOOL WINAPI IsWindowUnicode(HWND hwnd) {
    return TRUE;
}

BOOL WINAPI IsWindowVisible(HWND hwnd) {
    hostwd3d_window_info info;
    return windowInfo(hwnd, &info) && info.visible;
}

UINT_PTR WINAPI SetTimer(HWND hwnd, UINT_PTR id, UINT timeout, TIMERPROC proc) {
    hostwd3d_stub_once("SetTimer"); // the fullscreen WS_EX_TOPMOST restore timer (swapchain.c)
    return 0;
}

BOOL WINAPI KillTimer(HWND hwnd, UINT_PTR id) {
    return TRUE;
}

HCURSOR WINAPI LoadCursorA(HINSTANCE instance, LPCSTR name) {
    return NULL;
}

HICON WINAPI LoadIconA(HINSTANCE instance, LPCSTR name) {
    return NULL;
}

HANDLE WINAPI LoadImageA(HINSTANCE instance, LPCSTR name, UINT type, INT cx, INT cy, UINT flags) {
    return NULL;
}

BOOL WINAPI MoveWindow(HWND hwnd, INT x, INT y, INT width, INT height, BOOL repaint) {
    return hostwd3d_window_set_pos(HWND_ID(hwnd), x, y, width, height);
}

BOOL WINAPI SetWindowPos(HWND hwnd, HWND after, INT x, INT y, INT width, INT height, UINT flags) {
    // shortcut: only the host table changes; moving or resizing the guest's real window (fullscreen style/size
    // changes) needs a guest call in phase 2.
    if (!hostwd3d_window_get(HWND_ID(hwnd), NULL)) {
        return FALSE;
    }
    if (!(flags & SWP_NOMOVE) || !(flags & SWP_NOSIZE)) {
        hostwd3d_stub_once("SetWindowPos move/size");
        hostwd3d_window_set_pos(HWND_ID(hwnd), (flags & SWP_NOMOVE) ? -1 : x, (flags & SWP_NOMOVE) ? -1 : y,
            (flags & SWP_NOSIZE) ? -1 : width, (flags & SWP_NOSIZE) ? -1 : height);
    }
    if (flags & SWP_SHOWWINDOW) hostwd3d_window_set_visible(HWND_ID(hwnd), 1);
    if (flags & SWP_HIDEWINDOW) hostwd3d_window_set_visible(HWND_ID(hwnd), 0);
    return TRUE;
}

BOOL WINAPI ShowWindow(HWND hwnd, INT cmd) {
    return hostwd3d_window_set_visible(HWND_ID(hwnd), cmd != SW_HIDE);
}

ATOM WINAPI RegisterClassA(const WNDCLASSA* wc) {
    return 0xc001; // only the class name of wined3d's probe window
}

BOOL WINAPI UnregisterClassA(LPCSTR name, HINSTANCE instance) {
    return TRUE;
}

HHOOK WINAPI SetWindowsHookExW(INT id, HOOKPROC proc, HINSTANCE instance, DWORD tid) {
    // wined3d's WH_GETMESSAGE hook (wined3d_main.c) watches the device window's activation for fullscreen; no
    // messages reach the host, so it would never run.
    hostwd3d_stub_once("SetWindowsHookExW");
    return FAKE_HOOK;
}

BOOL WINAPI UnhookWindowsHookEx(HHOOK hook) {
    return TRUE;
}

BOOL WINAPI SystemParametersInfoW(UINT action, UINT uiParam, LPVOID param, UINT winIni) {
    switch (action) {
    case SPI_GETWORKAREA:
        screenRect((RECT*)param);
        return TRUE;
    case SPI_GETSCREENSAVEACTIVE:
        *(BOOL*)param = FALSE; // nothing to turn off around fullscreen
        return TRUE;
    case SPI_SETSCREENSAVEACTIVE:
        return TRUE;
    default:
        hostwd3d_stub_once("SystemParametersInfoW other action");
        return FALSE;
    }
}
