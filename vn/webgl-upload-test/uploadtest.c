// uploadtest.exe: textures through wined3d's WebGL upload, every pixel read back and compared with what the format
// says it must be. Catches: swapped channels, wrong alpha (X8's unused byte is garbage here and must read as 0xff),
// wrong row stride on a partial upload, a wrong tail on odd widths, a partial upload touching pixels outside its
// rectangle, and upload state (row length, alignment) leaking into the next upload.
// Prints "[upl] <case> ok|FAIL ... <hash>" per case and "[upl] done <failures>": the lines must not depend on where
// the conversion runs (nativeTextureUpload=0 vs on). Build: i686-w64-mingw32-gcc -O1 uploadtest.c -o uploadtest.exe -ld3d9
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

static IDirect3DDevice9* dev;
static int failures;

// Unique per position and case, with all four bytes varying, so a swap or a shifted row can't go unnoticed.
static BYTE pat(int x, int y, int ch, int seed) {
    DWORD v = (DWORD)(x * 2654435761u) ^ (DWORD)(y * 2246822519u) ^ (DWORD)((ch + 1) * 3266489917u) ^ (DWORD)(seed * 668265263u);
    return (BYTE)(v >> 19);
}
static DWORD patArgb(int x, int y, int seed) {
    return (DWORD)pat(x, y, 3, seed) << 24 | (DWORD)pat(x, y, 2, seed) << 16 | (DWORD)pat(x, y, 1, seed) << 8 | pat(x, y, 0, seed);
}
static DWORD hash(const void* p, size_t n, DWORD h) {
    const BYTE* b = p;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}

// Renders level `level` of tex with point sampling into a w x h A8R8G8B8 target and copies it out to a system memory surface.
static IDirect3DSurface9* readback(IDirect3DTexture9* tex, int level, int w, int h) {
    IDirect3DTexture9* rt = NULL;
    IDirect3DSurface9* rts = NULL, *mem = NULL, *old = NULL;
    struct { float x, y, z, rhw, u, v; } q[4] = {
        { -0.5f, -0.5f, 0, 1, 0, 0 }, { w - 0.5f, -0.5f, 0, 1, 1, 0 },
        { -0.5f, h - 0.5f, 0, 1, 0, 1 }, { w - 0.5f, h - 0.5f, 0, 1, 1, 1 },
    };
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &rt, NULL))) return NULL;
    IDirect3DTexture9_GetSurfaceLevel(rt, 0, &rts);
    IDirect3DDevice9_GetRenderTarget(dev, 0, &old);
    IDirect3DDevice9_SetRenderTarget(dev, 0, rts);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0x11223344, 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9*)tex);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, level ? D3DTEXF_POINT : D3DTEXF_NONE);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAXMIPLEVEL, level);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    IDirect3DDevice9_SetRenderTarget(dev, 0, old);
    IDirect3DSurface9_Release(old);
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &mem, NULL);
    IDirect3DDevice9_GetRenderTargetData(dev, rts, mem);
    IDirect3DSurface9_Release(rts);
    IDirect3DTexture9_Release(rt);
    return mem;
}

typedef DWORD (*Expect)(int x, int y, void* ctx);

static void check(const char* name, IDirect3DSurface9* mem, int w, int h, Expect expect, void* ctx) {
    D3DLOCKED_RECT lr;
    int bad = 0, firstX = -1, firstY = -1;
    DWORD got0 = 0, want0 = 0, h32 = 2166136261u;
    if (!mem || FAILED(IDirect3DSurface9_LockRect(mem, &lr, NULL, D3DLOCK_READONLY))) {
        printf("[upl] %s FAIL no readback\n", name); fflush(stdout); failures++;
        if (mem) IDirect3DSurface9_Release(mem);
        return;
    }
    for (int y = 0; y < h; y++) {
        const DWORD* row = (const DWORD*)((const BYTE*)lr.pBits + y * lr.Pitch);
        h32 = hash(row, w * 4, h32);
        for (int x = 0; x < w; x++) {
            DWORD want = expect(x, y, ctx);
            if (row[x] != want) {
                if (!bad++) { firstX = x; firstY = y; got0 = row[x]; want0 = want; }
            }
        }
    }
    IDirect3DSurface9_UnlockRect(mem);
    IDirect3DSurface9_Release(mem);
    if (bad) { printf("[upl] %s FAIL %d/%d first (%d,%d) got %08lx want %08lx hash %08lx\n", name, bad, w * h, firstX, firstY, got0, want0, h32); failures++; }
    else printf("[upl] %s ok hash %08lx\n", name, h32);
    fflush(stdout);
}

struct Ctx { int seed, seed2, rx, ry, rw, rh, opaque; };

static DWORD expect8888(int x, int y, void* p) {
    struct Ctx* c = p;
    DWORD v = patArgb(x, y, c->seed);
    return c->opaque ? v | 0xff000000 : v;
}
static DWORD expectPartial(int x, int y, void* p) {
    struct Ctx* c = p;
    if (x >= c->rx && x < c->rx + c->rw && y >= c->ry && y < c->ry + c->rh) {
        DWORD v = patArgb(x, y, c->seed2);
        return c->opaque ? v | 0xff000000 : v;
    }
    return expect8888(x, y, p);
}
static DWORD expect565(int x, int y, void* p) {
    struct Ctx* c = p;
    WORD v = (WORD)(pat(x, y, 0, c->seed) | pat(x, y, 1, c->seed) << 8);
    DWORD r = ((v >> 11) * 527 + 23) >> 6, g = (((v >> 5) & 63) * 259 + 33) >> 6, b = ((v & 31) * 527 + 23) >> 6;
    return 0xff000000 | r << 16 | g << 8 | b;
}
static DWORD expect1555(int x, int y, void* p) {
    struct Ctx* c = p;
    WORD v = (WORD)(pat(x, y, 0, c->seed) | pat(x, y, 1, c->seed) << 8);
    DWORD r = (((v >> 10) & 31) * 255 + 15) / 31, g = (((v >> 5) & 31) * 255 + 15) / 31, b = ((v & 31) * 255 + 15) / 31;
    return (v & 0x8000 ? 0xff000000u : 0) | r << 16 | g << 8 | b;
}

// Writes the 32-bit pattern into a locked rectangle at texture position (ox, oy). The X8 byte holds pattern too: never 0xff everywhere.
static void fill(const D3DLOCKED_RECT* lr, int ox, int oy, int w, int h, int seed) {
    for (int y = 0; y < h; y++) {
        DWORD* row = (DWORD*)((BYTE*)lr->pBits + y * lr->Pitch);
        for (int x = 0; x < w; x++) row[x] = patArgb(ox + x, oy + y, seed);
    }
}

static void fullUpload(const char* name, D3DFORMAT fmt, int opaque, int w, int h, int seed) {
    IDirect3DTexture9* t;
    D3DLOCKED_RECT lr;
    struct Ctx c = { .seed = seed, .opaque = opaque };
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, w, h, 1, 0, fmt, D3DPOOL_MANAGED, &t, NULL))) { printf("[upl] %s FAIL create\n", name); failures++; return; }
    IDirect3DTexture9_LockRect(t, 0, &lr, NULL, 0);
    fill(&lr, 0, 0, w, h, seed);
    IDirect3DTexture9_UnlockRect(t, 0);
    check(name, readback(t, 0, w, h), w, h, expect8888, &c);
    IDirect3DTexture9_Release(t);
}

// Partial upload with stride != width: a system memory surface's rectangle into a default pool texture filled before.
static void partialUpload(const char* name, D3DFORMAT fmt, int opaque, int seed) {
    enum { W = 128, H = 64 };
    IDirect3DTexture9* t, *base;
    IDirect3DSurface9* s, *d, *bs;
    D3DLOCKED_RECT lr;
    struct Ctx c = { .seed = seed, .seed2 = seed + 1, .rx = 5, .ry = 3, .rw = 37, .rh = 19, .opaque = opaque };
    RECT r = { c.rx + 13, c.ry + 7, c.rx + 13 + c.rw, c.ry + 7 + c.rh };
    POINT at = { c.rx, c.ry };
    IDirect3DDevice9_CreateTexture(dev, W, H, 1, 0, fmt, D3DPOOL_DEFAULT, &t, NULL);
    IDirect3DDevice9_CreateTexture(dev, W, H, 1, 0, fmt, D3DPOOL_SYSTEMMEM, &base, NULL);
    IDirect3DTexture9_LockRect(base, 0, &lr, NULL, 0);
    fill(&lr, 0, 0, W, H, seed);
    IDirect3DTexture9_UnlockRect(base, 0);
    IDirect3DDevice9_UpdateTexture(dev, (IDirect3DBaseTexture9*)base, (IDirect3DBaseTexture9*)t);
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H, fmt, D3DPOOL_SYSTEMMEM, &s, NULL);
    IDirect3DSurface9_LockRect(s, &lr, NULL, 0);
    // Source position (x + 13, y + 7) holds the pattern of destination (x, y).
    for (int y = 0; y < H; y++) {
        DWORD* row = (DWORD*)((BYTE*)lr.pBits + y * lr.Pitch);
        for (int x = 0; x < W; x++) row[x] = patArgb(x - 13, y - 7, seed + 1);
    }
    IDirect3DSurface9_UnlockRect(s);
    IDirect3DTexture9_GetSurfaceLevel(t, 0, &d);
    IDirect3DDevice9_UpdateSurface(dev, s, &r, d, &at);
    IDirect3DSurface9_Release(d);
    IDirect3DSurface9_Release(s);
    IDirect3DTexture9_GetSurfaceLevel(base, 0, &bs);
    IDirect3DSurface9_Release(bs);
    check(name, readback(t, 0, W, H), W, H, expectPartial, &c);
    IDirect3DTexture9_Release(base);
    IDirect3DTexture9_Release(t);
}

// A managed texture's dirty rectangle: the path a game's own partial lock takes.
static void dirtyRect(const char* name, D3DFORMAT fmt, int opaque, int seed) {
    enum { W = 100, H = 40 };
    IDirect3DTexture9* t;
    D3DLOCKED_RECT lr;
    struct Ctx c = { .seed = seed, .seed2 = seed + 1, .rx = 21, .ry = 9, .rw = 33, .rh = 11, .opaque = opaque };
    RECT r = { c.rx, c.ry, c.rx + c.rw, c.ry + c.rh };
    IDirect3DDevice9_CreateTexture(dev, W, H, 1, 0, fmt, D3DPOOL_MANAGED, &t, NULL);
    IDirect3DTexture9_LockRect(t, 0, &lr, NULL, 0);
    fill(&lr, 0, 0, W, H, seed);
    IDirect3DTexture9_UnlockRect(t, 0);
    IDirect3DSurface9_Release(readback(t, 0, W, H)); // uploads all of it first
    IDirect3DTexture9_LockRect(t, 0, &lr, &r, 0);
    fill(&lr, c.rx, c.ry, c.rw, c.rh, seed + 1);
    IDirect3DTexture9_UnlockRect(t, 0);
    check(name, readback(t, 0, W, H), W, H, expectPartial, &c);
    IDirect3DTexture9_Release(t);
}

static void mipLevel(const char* name, D3DFORMAT fmt, int opaque, int seed) {
    IDirect3DTexture9* t;
    D3DLOCKED_RECT lr;
    struct Ctx c = { .seed = seed, .opaque = opaque };
    IDirect3DDevice9_CreateTexture(dev, 64, 32, 2, 0, fmt, D3DPOOL_MANAGED, &t, NULL);
    IDirect3DTexture9_LockRect(t, 0, &lr, NULL, 0);
    fill(&lr, 0, 0, 64, 32, seed + 5);
    IDirect3DTexture9_UnlockRect(t, 0);
    IDirect3DTexture9_LockRect(t, 1, &lr, NULL, 0);
    fill(&lr, 0, 0, 32, 16, seed);
    IDirect3DTexture9_UnlockRect(t, 1);
    check(name, readback(t, 1, 32, 16), 32, 16, expect8888, &c);
    IDirect3DTexture9_Release(t);
}

static void upload16(const char* name, D3DFORMAT fmt, Expect expect, int w, int h, int seed) {
    IDirect3DTexture9* t;
    D3DLOCKED_RECT lr;
    struct Ctx c = { .seed = seed };
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, w, h, 1, 0, fmt, D3DPOOL_MANAGED, &t, NULL))) { printf("[upl] %s skipped: no format\n", name); return; }
    IDirect3DTexture9_LockRect(t, 0, &lr, NULL, 0);
    for (int y = 0; y < h; y++) {
        WORD* row = (WORD*)((BYTE*)lr.pBits + y * lr.Pitch);
        for (int x = 0; x < w; x++) row[x] = (WORD)(pat(x, y, 0, seed) | pat(x, y, 1, seed) << 8);
    }
    IDirect3DTexture9_UnlockRect(t, 0);
    check(name, readback(t, 0, w, h), w, h, expect, &c);
    IDirect3DTexture9_Release(t);
}

int main(void) {
    static const struct { int w, h; } sizes[] = { {1, 1}, {3, 2}, {4, 4}, {5, 3}, {17, 5}, {64, 64}, {255, 3}, {800, 600}, {1024, 768} };
    char name[64];
    WNDCLASSA wc = { .lpfnWndProc = DefWindowProcA, .hInstance = GetModuleHandleA(NULL), .lpszClassName = "upl" };
    D3DPRESENT_PARAMETERS pp = { .BackBufferWidth = 800, .BackBufferHeight = 600, .BackBufferFormat = D3DFMT_X8R8G8B8, .BackBufferCount = 1,
        .SwapEffect = D3DSWAPEFFECT_DISCARD, .Windowed = TRUE };
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    HWND win;
    HRESULT hr;
    RegisterClassA(&wc);
    win = CreateWindowA("upl", "upl", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 800, 600, NULL, NULL, wc.hInstance, NULL);
    pp.hDeviceWindow = win;
    if (!d3d) { printf("[upl] done no d3d9\n"); return 1; }
    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, win, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) { printf("[upl] done CreateDevice %08lx\n", hr); fflush(stdout); return 1; }

    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        sprintf(name, "x8r8g8b8-%dx%d", sizes[i].w, sizes[i].h);
        fullUpload(name, D3DFMT_X8R8G8B8, 1, sizes[i].w, sizes[i].h, 100 + i);
        sprintf(name, "a8r8g8b8-%dx%d", sizes[i].w, sizes[i].h);
        fullUpload(name, D3DFMT_A8R8G8B8, 0, sizes[i].w, sizes[i].h, 200 + i);
    }
    partialUpload("x8r8g8b8-partial", D3DFMT_X8R8G8B8, 1, 300);
    upload16("r5g6b5-after-partial", D3DFMT_R5G6B5, expect565, 33, 9, 310);
    partialUpload("a8r8g8b8-partial", D3DFMT_A8R8G8B8, 0, 320);
    upload16("a1r5g5b5-after-partial", D3DFMT_A1R5G5B5, expect1555, 33, 9, 330);
    dirtyRect("x8r8g8b8-dirty", D3DFMT_X8R8G8B8, 1, 340);
    dirtyRect("a8r8g8b8-dirty", D3DFMT_A8R8G8B8, 0, 350);
    fullUpload("a8r8g8b8-after-dirty", D3DFMT_A8R8G8B8, 0, 31, 7, 355);
    mipLevel("x8r8g8b8-mip1", D3DFMT_X8R8G8B8, 1, 360);
    mipLevel("a8r8g8b8-mip1", D3DFMT_A8R8G8B8, 0, 370);

    printf("[upl] done %d failures\n", failures); fflush(stdout);
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    return failures != 0;
}
