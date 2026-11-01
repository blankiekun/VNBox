/* GetGlyphOutline conformance dump for the win32u glyph cache (vn/wine/win32u-glyph-cache.patch).
 * For many fonts, chars, formats and matrices it calls GetGlyphOutline as a size query, with a big
 * buffer, a short buffer and an exact buffer (each prefilled with a pattern), and prints the return
 * value, the metrics and a hash of the whole buffer, in two passes. Pass 2 hits the cache, pass 1
 * mostly fills it. The results must match between the passes (checked here, "MISMATCH" lines) and
 * between a run with the cache and one with the root fs's win32u.so (compare the printed per-font
 * hashes of all pass 1 results). Prints to stdout (the player console in VNBox).
 * Build: i686-w64-mingw32-gcc -O2 -o ggotest.exe ggotest.c -lgdi32 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static unsigned fnv( const BYTE *p, DWORD n )
{
    unsigned h = 2166136261u;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

static BYTE buf[1 << 20];
static char line[512];
static char *pass1[400000];
static int nlines, cur, mismatches, total;
static unsigned face_hash;

static void emit( int pass )
{
    total++;
    if (pass == 1)
    {
        if (nlines < 400000) pass1[nlines++] = _strdup( line );
        face_hash = fnv( (const BYTE *)line, strlen( line ) ) ^ face_hash * 31;
    }
    else if (cur >= nlines || strcmp( pass1[cur++], line ))
    {
        if (++mismatches <= 20) printf( "MISMATCH %s | %s\n", cur <= nlines ? pass1[cur - 1] : "-", line );
    }
}

static void call( int pass, HDC hdc, const char *tag, UINT ch, UINT fmt, const MAT2 *m, DWORD len, BYTE fill )
{
    GLYPHMETRICS gm;
    DWORD ret;
    memset( &gm, 0x5a, sizeof(gm) );
    memset( buf, fill, len + 64 );
    ret = GetGlyphOutlineW( hdc, ch, fmt, &gm, len, len ? buf : NULL, m );
    _snprintf( line, sizeof(line), "%s ch=%04x fmt=%x len=%lu ret=%ld gm=%u,%u,%ld,%ld,%d,%d buf=%08x",
               tag, ch, fmt, len, (long)ret, gm.gmBlackBoxX, gm.gmBlackBoxY, gm.gmptGlyphOrigin.x,
               gm.gmptGlyphOrigin.y, gm.gmCellIncX, gm.gmCellIncY, fnv( buf, len + 64 ) );
    emit( pass );
}

static FIXED fx( double d ) { FIXED f; LONG l = (LONG)(d * 65536.0); memcpy( &f, &l, 4 ); return f; }

int main( void )
{
    static const WCHAR *faces[] = { L"IPAGothic", L"IPAPGothic", L"IPAMincho", L"IPAPMincho", L"MS Gothic",
                                    L"ＭＳ 明朝", L"@MS Gothic", L"@IPAMincho", L"Tahoma", L"MS Sans Serif",
                                    L"System", L"Courier", L"Small Fonts" };
    static const int heights[] = { -12, 25 };
    static const UINT fmts[] = { GGO_BITMAP, GGO_GRAY2_BITMAP, GGO_GRAY4_BITMAP, GGO_GRAY8_BITMAP,
                                 GGO_GRAY8_BITMAP | GGO_UNHINTED, GGO_BITMAP | GGO_UNHINTED,
                                 GGO_METRICS, GGO_NATIVE, GGO_GRAY4_BITMAP | GGO_GLYPH_INDEX,
                                 GGO_BITMAP | GGO_GLYPH_INDEX };
    static const WCHAR text[] = L" !09AMWgjy~\x00e9\x3042\x3044\x3093\x30a2\x30f3\x30fc\x6f22\x5b57\x65e5\x672c\x8a9e"
                                L"\x3001\x3002\x300c\x300d\xff01\xff21\x3000\x2026\x4e00";
    MAT2 mats[4];
    HDC hdc = CreateCompatibleDC( 0 );
    int pass, f, h, w, i, k, c;

    memset( mats, 0, sizeof(mats) );
    mats[0].eM11 = fx( 1 ); mats[0].eM22 = fx( 1 );                                   /* identity */
    mats[1].eM11 = fx( 2 ); mats[1].eM22 = fx( 1.5 );                                 /* scaled */
    mats[2].eM11 = fx( 0.8660254 ); mats[2].eM12 = fx( 0.5 );
    mats[2].eM21 = fx( -0.5 ); mats[2].eM22 = fx( 0.8660254 );                        /* rotated 30 */
    mats[3].eM11 = fx( 1 ); mats[3].eM21 = fx( 0.3 ); mats[3].eM22 = fx( 1 );        /* skewed */

    for (pass = 1; pass <= 2; pass++)
    {
        cur = 0;
        for (f = 0; f < ARRAYSIZE(faces); f++, face_hash = 0)
        for (h = 0; h < ARRAYSIZE(heights); h++)
        for (w = 0; w < 3; w++)  /* normal, bold, italic */
        {
            char tag[160];
            HFONT font = CreateFontW( heights[h], 0, 0, 0, w == 1 ? FW_BOLD : FW_NORMAL, w == 2, 0, 0,
                                      DEFAULT_CHARSET, 0, 0, w == 0 ? ANTIALIASED_QUALITY : DEFAULT_QUALITY,
                                      0, faces[f] );
            HFONT old = SelectObject( hdc, font );
            for (i = 0; i < ARRAYSIZE(fmts); i++)
            for (k = 0; k < ARRAYSIZE(mats); k++)
            for (c = 0; c < ARRAYSIZE(text) - 1; c++)
            {
                UINT fmt = fmts[i], ch = (fmt & GGO_GLYPH_INDEX) ? c * 7 : text[c];
                GLYPHMETRICS gm;
                DWORD need;
                if (k && i > 2) continue;  /* matrices with GGO_BITMAP, GRAY2 and GRAY4 */
                _snprintf( tag, sizeof(tag), "f=%d h=%d w=%d m=%d", f, heights[h], w, k );
                /* buffer first for half the chars (in pass 1 this fills the cache from a buffer call) */
                if (c & 1) call( pass, hdc, tag, ch, fmt, &mats[k], 65536, 0xcc );
                call( pass, hdc, tag, ch, fmt, &mats[k], 0, 0x11 );
                need = GetGlyphOutlineW( hdc, ch, fmt, &gm, 0, NULL, &mats[k] );
                if (need == GDI_ERROR || need > 60000) continue;
                if (need) call( pass, hdc, tag, ch, fmt, &mats[k], need - 1, 0xab );  /* short */
                call( pass, hdc, tag, ch, fmt, &mats[k], need, 0x77 );                /* exact */
                call( pass, hdc, tag, ch, fmt, &mats[k], need + 37, 0xcc );           /* bigger: tail */
            }
            SelectObject( hdc, old );
            DeleteObject( font );
            if (pass == 1 && h == ARRAYSIZE(heights) - 1 && w == 2)
            {
                printf( "FACE %d lines=%d hash=%08x\n", f, nlines, face_hash );
                fflush( stdout );
            }
        }
    }
    printf( "DONE calls=%d mismatches=%d\n", total, mismatches );
    fflush( stdout );
    return mismatches != 0;
}
