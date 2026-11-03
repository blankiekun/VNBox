/* Minimal stand-in for Wine 11.0's dlls/win32u/ntgdi_private.h (LGPL-2.1+, as primitives.c): the types and helpers
 * dibdrv/primitives.c and dibdrv.h use, copied from Wine's headers. */
#ifndef VNBOX_DIBDRV_SHIM_H
#define VNBOX_DIBDRV_SHIM_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef int INT;
typedef unsigned int UINT;
typedef int BOOL;
typedef unsigned short USHORT;
typedef unsigned short COLOR16;
typedef DWORD COLORREF;
typedef const unsigned short *LPCWSTR;
typedef void *HRGN, *HBRUSH, *HFONT, *HPEN;
#define TRUE 1
#define FALSE 0
typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { LONG x, y; } POINT;
typedef struct { BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved; } RGBQUAD;
typedef struct { BYTE BlendOp, BlendFlags, SourceConstantAlpha, AlphaFormat; } BLENDFUNCTION;
typedef struct { LONG x, y; COLOR16 Red, Green, Blue, Alpha; } TRIVERTEX;
typedef struct BITMAPINFO BITMAPINFO;
struct gdi_image_bits { void *ptr; BOOL is_copy; void (*free)(struct gdi_image_bits *); void *param; };
struct brush_pattern { BITMAPINFO *info; struct gdi_image_bits bits; UINT usage; };
struct bitblt_coords { int log_x, log_y, log_width, log_height, x, y, width, height; RECT visrect; DWORD layout; };
struct font_gamma_ramp { UINT gamma; BYTE encode[256]; BYTE decode[256]; };
struct gdi_physdev { void *funcs, *next; void *hdc; };
typedef struct gdi_physdev *PHYSDEV;
#define RGB(r,g,b) ((COLORREF)(((BYTE)(r)|((WORD)((BYTE)(g))<<8))|(((DWORD)(BYTE)(b))<<16)))
#define GetRValue(rgb) ((BYTE)(rgb))
#define GetGValue(rgb) ((BYTE)(((WORD)(rgb)) >> 8))
#define GetBValue(rgb) ((BYTE)((rgb)>>16))
#define min(a,b) (((a) < (b)) ? (a) : (b))
#define max(a,b) (((a) > (b)) ? (a) : (b))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
typedef struct { LONG cx, cy; } SIZE;
typedef struct DC DC;
typedef struct BITMAPOBJ BITMAPOBJ;
#define R2_BLACK 1
#define R2_NOTMERGEPEN 2
#define R2_MASKNOTPEN 3
#define R2_NOTCOPYPEN 4
#define R2_MASKPENNOT 5
#define R2_NOT 6
#define R2_XORPEN 7
#define R2_NOTMASKPEN 8
#define R2_MASKPEN 9
#define R2_NOTXORPEN 10
#define R2_NOP 11
#define R2_MERGENOTPEN 12
#define R2_COPYPEN 13
#define R2_MERGEPENNOT 14
#define R2_MERGEPEN 15
#define R2_WHITE 16
#define BLACKONWHITE 1
#define WHITEONBLACK 2
#define COLORONCOLOR 3
#define HALFTONE 4
#define STRETCH_ANDSCANS BLACKONWHITE
#define STRETCH_ORSCANS WHITEONBLACK
#define STRETCH_DELETESCANS COLORONCOLOR
#define STRETCH_HALFTONE HALFTONE
#define BI_RGB 0
#define BI_RLE8 1
#define BI_RLE4 2
#define BI_BITFIELDS 3
#define AC_SRC_OVER 0x00
#define AC_SRC_ALPHA 0x01
#define GRADIENT_FILL_RECT_H 0x00000000
#define GRADIENT_FILL_RECT_V 0x00000001
#define GRADIENT_FILL_TRIANGLE 0x00000002
#define GRADIENT_FILL_OP_FLAG 0x000000ff
static inline BOOL IsRectEmpty(const RECT *rect)
{
    if (!rect) return TRUE;
    return ((rect->left >= rect->right) || (rect->top >= rect->bottom));
}
extern const RGBQUAD *get_default_color_table( int bpp );
typedef int64_t INT64;
typedef uint64_t UINT64;
#define LOWORD(l) ((WORD)((DWORD)(l) & 0xFFFF))
#define HIWORD(l) ((WORD)((DWORD)(l) >> 16))
static inline BOOL OffsetRect(RECT *rect, INT x, INT y)
{
    if (!rect) return FALSE;
    rect->left   += x;
    rect->right  += x;
    rect->top    += y;
    rect->bottom += y;
    return TRUE;
}
static inline BOOL intersect_rect( RECT *dst, const RECT *src1, const RECT *src2 )
{
    dst->left   = max( src1->left, src2->left );
    dst->top    = max( src1->top, src2->top );
    dst->right  = min( src1->right, src2->right );
    dst->bottom = min( src1->bottom, src2->bottom );
    return !IsRectEmpty( dst );
}
static inline void get_bounding_rect( RECT *rect, int x, int y, int width, int height )
{
    rect->left   = x;
    rect->right  = x + width;
    rect->top    = y;
    rect->bottom = y + height;
    if (rect->left > rect->right)
    {
        int tmp = rect->left;
        rect->left = rect->right + 1;
        rect->right = tmp + 1;
    }
    if (rect->top > rect->bottom)
    {
        int tmp = rect->top;
        rect->top = rect->bottom + 1;
        rect->bottom = tmp + 1;
    }
}

#endif
