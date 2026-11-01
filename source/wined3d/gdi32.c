/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

// Host wined3d: the gdi32 calls wined3d makes. Pixel formats are real (Boxedwine's GL formats); DCs are the window
// DCs and memory DCs of hostwd3d_bridge.h. GDI drawing (BitBlt, bitmaps, DIB color tables: wined3d's
// GetDC-on-a-surface path, mostly ddraw) and gamma ramps are refused or ignored here, each logging once; they are
// phase 3 (ddraw) work. The D3DKMT* entry points are not here (PHASE1-A's display shims).
#include <stdarg.h>
#include <string.h>
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "hostwd3d_bridge.h"

#define HDC_ID(dc) ((uint32_t)(ULONG_PTR)(dc))

C_ASSERT(sizeof(PIXELFORMATDESCRIPTOR) == 40); /* crosses the bridge as Boxedwine's 40-byte PixelFormat */

INT WINAPI ChoosePixelFormat(HDC dc, const PIXELFORMATDESCRIPTOR* pfd) {
    return pfd ? hostwd3d_pf_choose(pfd) : 0;
}

INT WINAPI DescribePixelFormat(HDC dc, int format, UINT size, LPPIXELFORMATDESCRIPTOR pfd) {
    int count = hostwd3d_pf_count();
    if (!pfd || !format) {
        return count; // DescribePixelFormat(dc, 0, 0, NULL) asks for the count
    }
    if (size < sizeof(*pfd) || !hostwd3d_pf_describe(format, pfd)) {
        return 0;
    }
    return count;
}

INT WINAPI GetPixelFormat(HDC dc) {
    return hostwd3d_dc_get_pixel_format(HDC_ID(dc));
}

BOOL WINAPI SetPixelFormat(HDC dc, int format, const PIXELFORMATDESCRIPTOR* pfd) {
    // Win32: a window's pixel format can be set once; setting the same one again succeeds.
    int current = hostwd3d_dc_get_pixel_format(HDC_ID(dc));
    if (current) {
        return current == format;
    }
    return hostwd3d_dc_set_pixel_format(HDC_ID(dc), format);
}

HDC WINAPI CreateCompatibleDC(HDC dc) {
    return (HDC)(ULONG_PTR)hostwd3d_dc_create_memory();
}

HDC WINAPI CreateDCW(LPCWSTR driver, LPCWSTR device, LPCWSTR output, const DEVMODEW* mode) {
    // wined3d opens the display device by name only to get/set its gamma ramp (directx.c)
    return (HDC)(ULONG_PTR)hostwd3d_dc_create_memory();
}

BOOL WINAPI DeleteDC(HDC dc) {
    return hostwd3d_dc_delete(HDC_ID(dc));
}

BOOL WINAPI GetDeviceGammaRamp(HDC dc, LPVOID ramp) {
    // An identity ramp: three 256-entry WORD tables.
    WORD* r = (WORD*)ramp;
    for (int i = 0; i < 256; i++) {
        r[i] = r[256 + i] = r[512 + i] = (WORD)(i * 257);
    }
    return TRUE;
}

BOOL WINAPI SetDeviceGammaRamp(HDC dc, LPVOID ramp) {
    hostwd3d_stub_once("SetDeviceGammaRamp"); // the browser has no gamma; D3D brightness sliders do nothing
    return TRUE;
}

BOOL WINAPI BitBlt(HDC dst, INT x, INT y, INT width, INT height, HDC src, INT srcX, INT srcY, DWORD rop) {
    hostwd3d_stub_once("BitBlt");
    return FALSE;
}

BOOL WINAPI StretchBlt(HDC dst, INT x, INT y, INT width, INT height, HDC src, INT srcX, INT srcY, INT srcWidth, INT srcHeight, DWORD rop) {
    hostwd3d_stub_once("StretchBlt");
    return FALSE;
}

HBITMAP WINAPI CreateBitmap(INT width, INT height, UINT planes, UINT bpp, LPCVOID bits) {
    hostwd3d_stub_once("CreateBitmap");
    return NULL;
}

BOOL WINAPI DeleteObject(HGDIOBJ obj) {
    return obj != NULL;
}

INT WINAPI GetObjectA(HGDIOBJ obj, INT size, LPVOID buffer) {
    hostwd3d_stub_once("GetObjectA");
    return 0;
}

HGDIOBJ WINAPI SelectObject(HDC dc, HGDIOBJ obj) {
    hostwd3d_stub_once("SelectObject");
    return NULL;
}

UINT WINAPI SetDIBColorTable(HDC dc, UINT start, UINT count, const RGBQUAD* colors) {
    hostwd3d_stub_once("SetDIBColorTable"); // 8-bit ddraw surfaces' palettes: phase 3
    return 0;
}
