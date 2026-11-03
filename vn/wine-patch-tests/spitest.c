/* Checks vn/wine/win32u-spi-cache.patch (the cached SPI_GETNONCLIENTMETRICS result). Run by hand like ggotest, twice:
 * with the cache and with the URL option cheapSpi=0 (the plain Wine paths). FAIL lines are wrong in either run, and the
 * "[spi] hash" lines must be the same in both runs.
 * Bugs it catches:
 *   - a cache hit touching the 4 bytes past a short struct (the struct ends at a no-access page here);
 *   - a SET that leaves the old result in the cache (a font added or removed only reaches the invalidation here: the
 *     measured result does not change, so that case relies on the hashes matching the cheapSpi=0 run);
 *   - a result cached under one DPI awareness context returned under another.
 * Build: i686-w64-mingw32-gcc -O2 -D_WIN32_WINNT=0x0A00 -o spitest.exe spitest.c -lgdi32 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define NCM_OLD FIELD_OFFSET(NONCLIENTMETRICSW, iPaddedBorderWidth)
static const WCHAR font[] = L"Z:\\opt\\wine\\share\\wine\\fonts\\ipag.ttf";  /* an IPA font of jp.zip */
static int fails;

static void check( int ok, const char *what )
{
    printf( "[spi] %s %s\n", ok ? "ok" : "FAIL", what );
    fflush( stdout );
    if (!ok) fails++;
}

static unsigned fnv( const void *p, DWORD n )
{
    const BYTE *b = p;
    unsigned h = 2166136261u;
    while (n--) h = (h ^ *b++) * 16777619u;
    return h;
}

static BOOL get( NONCLIENTMETRICSW *ncm, UINT cb )
{
    memset( ncm, 0xa5, sizeof(*ncm) );
    ncm->cbSize = cb;
    return SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, cb, ncm, 0 );
}

static void print_hash( const char *what )
{
    NONCLIENTMETRICSW ncm;
    BOOL ret = get( &ncm, sizeof(ncm) );
    printf( "[spi] hash %s ret=%d %08x\n", what, ret, fnv( &ncm, sizeof(ncm) ) );
    fflush( stdout );
}

/* the bug's case: the cache is filled, then a short struct ends right before a no-access page */
static void short_struct(void)
{
    typedef BOOL (WINAPI *fordpi_t)( UINT, UINT, void *, UINT, UINT );
    fordpi_t for_dpi = (fordpi_t)GetProcAddress( GetModuleHandleA( "user32" ), "SystemParametersInfoForDpi" );
    NONCLIENTMETRICSW full, *ncm;
    DWORD old;
    BYTE *page = VirtualAlloc( NULL, 0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );

    VirtualProtect( page + 0x1000, 0x1000, PAGE_NOACCESS, &old );
    ncm = (NONCLIENTMETRICSW *)(page + 0x1000 - NCM_OLD);

    get( &full, sizeof(full) );  /* fills the cache */
    get( &full, sizeof(full) );
    memset( ncm, 0xa5, NCM_OLD );
    ncm->cbSize = NCM_OLD;
    /* the fault is caught in the Wine syscall: the bug shows as a return value of STATUS_ACCESS_VIOLATION, struct unfilled */
    check( SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, NCM_OLD, ncm, 0 ) == TRUE, "short struct at a page end returns TRUE" );
    check( ncm->cbSize == NCM_OLD, "short struct keeps its cbSize" );
    check( !memcmp( &ncm->iBorderWidth, &full.iBorderWidth, NCM_OLD - sizeof(UINT) ), "short struct has the full struct's fields" );
    if (for_dpi)
    {
        for_dpi( SPI_GETNONCLIENTMETRICS, sizeof(full), &full, 0, 96 );
        memset( ncm, 0xa5, NCM_OLD );
        ncm->cbSize = NCM_OLD;
        check( for_dpi( SPI_GETNONCLIENTMETRICS, NCM_OLD, ncm, 0, 96 ) == TRUE, "ForDpi short struct at a page end returns TRUE" );
    }
}

static void set_invalidates(void)
{
    NONCLIENTMETRICSW before, changed, after;

    get( &before, sizeof(before) );
    get( &before, sizeof(before) );
    changed = before;
    changed.iMenuHeight = before.iMenuHeight + 7;
    check( SystemParametersInfoW( SPI_SETNONCLIENTMETRICS, sizeof(changed), &changed, 0 ), "SET a changed menu height" );
    get( &after, sizeof(after) );
    check( after.iMenuHeight == changed.iMenuHeight, "GET after SET returns the new value" );
    check( SystemParametersInfoW( SPI_SETNONCLIENTMETRICS, sizeof(before), &before, 0 ), "SET the old value back" );
    get( &after, sizeof(after) );
    check( after.iMenuHeight == before.iMenuHeight, "GET after the second SET returns the old value" );
}

static void dpi_contexts(void)
{
    typedef HANDLE (WINAPI *setctx_t)( HANDLE );
    static const struct { HANDLE ctx; const char *name; } ctxs[] = {
        { DPI_AWARENESS_CONTEXT_UNAWARE, "unaware" }, { DPI_AWARENESS_CONTEXT_SYSTEM_AWARE, "system" },
        { DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE, "permonitor" }, { DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, "permonitor2" } };
    setctx_t set_ctx = (setctx_t)GetProcAddress( GetModuleHandleA( "user32" ), "SetThreadDpiAwarenessContext" );
    int i, pass;

    if (!set_ctx) { printf( "[spi] no SetThreadDpiAwarenessContext, skipped\n" ); return; }
    /* more contexts than cache slots would evict each other: the second pass is a mix of hits and misses */
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < ARRAYSIZE(ctxs); i++)
        {
            HANDLE old = set_ctx( ctxs[i].ctx );
            print_hash( ctxs[i].name );
            set_ctx( old );
        }
}

/* The font only has to reach the invalidation: normalize_nonclientmetrics measures whatever is installed then. */
static void font_invalidates(void)
{
    NONCLIENTMETRICSW before, after;
    int added;

    get( &before, sizeof(before) );
    get( &before, sizeof(before) );
    added = AddFontResourceW( font );
    printf( "[spi] AddFontResource %d\n", added );
    print_hash( "after-add" );
    printf( "[spi] RemoveFontResource %d\n", RemoveFontResourceW( font ) );
    print_hash( "after-remove" );
    get( &after, sizeof(after) );
    check( !memcmp( &after, &before, sizeof(after) ), "same result after adding and removing a font" );
}

int main(void)
{
    print_hash( "start" );
    short_struct();
    set_invalidates();
    dpi_contexts();
    font_invalidates();
    printf( "[spi] done fails=%d\n", fails );
    fflush( stdout );
    return fails != 0;
}
