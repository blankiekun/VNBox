/* The guest wined3d.dll's own code (the exports are generated: wined3d_thunk.S).
 *
 * Callbacks: host wined3d never calls guest code. It queues what it would have called (hostwd3d_callbacks.h) and
 * sets _wd3d_pending; the export's tail then calls wd3d_run_callbacks, which takes them one at a time (id 2) and makes
 * each call here, in order. One at a time keeps the order when a callback calls wined3d and queues more.
 *
 * Windows: host wined3d only knows windows registered with id 1 (guest HWND, winex11's X window, client rect on
 * the screen, visible, client origin in the window rect and its size). Exports that pass wined3d a window register it
 * first (the generated prologues).
 *
 * GDI: wined3d_texture_get_dc's HDC is a host one; the guest makes its own DC over the same texture memory
 * (wd3d_get_dc_post) and keeps the pair until the host says the host DC is gone (CB_DC_DESTROYED).
 *
 * Built with i686-w64-mingw32-gcc -nostdlib (no CRT): only user32, gdi32 and kernel32 are imported. */
#include <windows.h>

/* ddk/d3dkmthk.h (not in MinGW's headers) */
typedef struct
{
    void *pMemory;
    UINT Format; /* D3DDDIFORMAT */
    UINT Width, Height, Pitch;
    HDC hDeviceDc;
    PALETTEENTRY *pColorTable;
    HDC hDc;
    HANDLE hBitmap;
} D3DKMT_CREATEDCFROMMEMORY;

typedef struct
{
    HDC hDc;
    HANDLE hBitmap;
} D3DKMT_DESTROYDCFROMMEMORY;

LONG WINAPI D3DKMTCreateDCFromMemory(D3DKMT_CREATEDCFROMMEMORY *desc);
LONG WINAPI D3DKMTDestroyDCFromMemory(const D3DKMT_DESTROYDCFROMMEMORY *desc);

struct hostwd3d_callback
{
    UINT kind, target, arg, value;
};

enum
{
    CB_OBJECT_DESTROYED = 1,
    CB_DEVICE_CREATED,
    CB_MODE_CHANGED,
    CB_ACTIVATE,
    CB_WINDOWED_STATE_CHANGED,
    CB_SUB_RESOURCE_CREATED,
    CB_DC_DESTROYED,
};

/* the host calls (wined3d_thunk.S) */
UINT __cdecl wd3d_drain_call(struct hostwd3d_callback *out, UINT max);
UINT __cdecl wd3d_dc_info_call(UINT host_dc, UINT info[5]); /* {guest memory, D3DDDIFORMAT, width, height, pitch} */
static void dc_destroyed(UINT host_dc);
HRESULT __cdecl wined3d_texture_release_dc(void *texture, UINT sub_resource_idx, HDC dc); /* the export */
UINT __cdecl wd3d_register_window_call(HWND hwnd, UINT drawable, LONG x, LONG y, LONG width, LONG height, UINT visible,
        LONG frame_left, LONG frame_top, LONG window_width, LONG window_height);

typedef void (__stdcall *object_destroyed_fn)(void *parent);
typedef void (__cdecl *device_created_fn)(void *device_parent, void *device);
typedef void (__cdecl *mode_changed_fn)(void *device_parent);
typedef void (__cdecl *activate_fn)(void *device_parent, BOOL activate);
typedef void (__cdecl *windowed_state_changed_fn)(void *state_parent, BOOL windowed);

/* struct { const ops *ops; } -> ops[index] */
static void *op(UINT object, unsigned int index)
{
    void **ops = *(void ***)object;
    return ops[index];
}

void __cdecl wd3d_run_callbacks(void)
{
    struct hostwd3d_callback cb;

    while (wd3d_drain_call(&cb, 1))
    {
        switch (cb.kind)
        {
        case CB_OBJECT_DESTROYED: /* target = the guest's wined3d_parent_ops */
            (*(object_destroyed_fn *)cb.target)((void *)cb.arg);
            break;
        case CB_DEVICE_CREATED:
            ((device_created_fn)op(cb.target, 0))((void *)cb.target, (void *)cb.value);
            break;
        case CB_MODE_CHANGED:
            ((mode_changed_fn)op(cb.target, 1))((void *)cb.target);
            break;
        case CB_ACTIVATE:
            ((activate_fn)op(cb.target, 2))((void *)cb.target, cb.value);
            break;
        case CB_WINDOWED_STATE_CHANGED:
            ((windowed_state_changed_fn)op(cb.target, 0))((void *)cb.target, cb.value);
            break;
        case CB_DC_DESTROYED:
            dc_destroyed(cb.value);
            break;
        default: /* CB_SUB_RESOURCE_CREATED: answered on the host */
            break;
        }
    }
}

/* d3d9, d3d8 and ddraw use the guest's own wined3d (C:\webgl's, installed as wined3d-webgl.dll by startupArgs.cpp):
 * host wined3d forwards d3d11/dxgi's calls only. Their wined3d.dll imports are pointed at its exports, when such a
 * DLL is mapped (before its DllMain: an ntdll load notification) or already is (it is what loaded this stub). Two
 * wined3d's then share the process: each object stays in the one that made it. */
static const char *const guest_importers[] = {"d3d9.dll", "d3d8.dll", "ddraw.dll"};
static HMODULE stub_module, guest_wined3d;

static IMAGE_NT_HEADERS *nt_headers(HMODULE module)
{
    return (IMAGE_NT_HEADERS *)((BYTE *)module + ((IMAGE_DOS_HEADER *)module)->e_lfanew);
}

static int same_name_ci(const char *a, const char *b)
{
    char x, y;

    do
    {
        x = *a++;
        y = *b++;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
    } while (x == y && x);
    return x == y;
}

/* the name of this stub's export at address, or NULL */
static const char *stub_export_name(ULONG_PTR address)
{
    BYTE *base = (BYTE *)stub_module;
    IMAGE_EXPORT_DIRECTORY *exports = (IMAGE_EXPORT_DIRECTORY *)(base
            + nt_headers(stub_module)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
    DWORD *functions = (DWORD *)(base + exports->AddressOfFunctions), *names = (DWORD *)(base + exports->AddressOfNames);
    WORD *ordinals = (WORD *)(base + exports->AddressOfNameOrdinals);
    DWORD i;

    for (i = 0; i < exports->NumberOfNames; ++i)
        if ((ULONG_PTR)(base + functions[ordinals[i]]) == address)
            return (const char *)(base + names[i]);
    return NULL;
}

static void use_guest_wined3d(HMODULE module)
{
    IMAGE_DATA_DIRECTORY *dir = &nt_headers(module)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *desc;
    ULONG_PTR *start, *iat, *end;
    const char *name;
    DWORD old;
    void *proc;

    if (!dir->VirtualAddress)
        return;
    for (desc = (IMAGE_IMPORT_DESCRIPTOR *)((BYTE *)module + dir->VirtualAddress); desc->Name; ++desc)
    {
        if (!same_name_ci((const char *)module + desc->Name, "wined3d.dll"))
            continue;
        if (!guest_wined3d && !(guest_wined3d = LoadLibraryA("wined3d-webgl.dll")))
            return;
        for (end = start = (ULONG_PTR *)((BYTE *)module + desc->FirstThunk); *end; ++end);
        if (end == start || !VirtualProtect(start, (BYTE *)end - (BYTE *)start, PAGE_READWRITE, &old))
            continue;
        for (iat = start; iat < end; ++iat)
            if ((name = stub_export_name(*iat)) && (proc = GetProcAddress(guest_wined3d, name)))
                *iat = (ULONG_PTR)proc;
        VirtualProtect(start, (BYTE *)end - (BYTE *)start, old, &old);
    }
}

typedef struct
{
    USHORT Length, MaximumLength;
    const WCHAR *Buffer;
} wd3d_unicode_string;

typedef struct /* LDR_DLL_LOADED_NOTIFICATION_DATA */
{
    ULONG Flags;
    const wd3d_unicode_string *FullDllName, *BaseDllName;
    void *DllBase;
    ULONG SizeOfImage;
} wd3d_dll_notification;

static void CALLBACK dll_notification(ULONG reason, const wd3d_dll_notification *data, void *context)
{
    unsigned int i, j, n;
    const char *want;

    if (reason != 1 || !data->BaseDllName) /* LDR_DLL_NOTIFICATION_REASON_LOADED */
        return;
    n = data->BaseDllName->Length / sizeof(WCHAR);
    for (i = 0; i < sizeof(guest_importers) / sizeof(guest_importers[0]); ++i)
    {
        want = guest_importers[i];
        for (j = 0; j < n && want[j]; ++j)
        {
            WCHAR c = data->BaseDllName->Buffer[j];
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c != (BYTE)want[j])
                break;
        }
        if (j == n && !want[j])
            use_guest_wined3d((HMODULE)data->DllBase);
    }
}

typedef LONG (WINAPI *register_dll_notification_fn)(ULONG flags, void *callback, void *context, void **cookie);

void __cdecl wd3d_process_attach(HMODULE module)
{
    register_dll_notification_fn reg;
    HMODULE importer;
    unsigned int i;
    void *cookie;

    stub_module = module;
    if ((reg = (register_dll_notification_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "LdrRegisterDllNotification")))
        reg(0, dll_notification, NULL, &cookie);
    for (i = 0; i < sizeof(guest_importers) / sizeof(guest_importers[0]); ++i)
        if ((importer = GetModuleHandleA(guest_importers[i])))
            use_guest_wined3d(importer);
}

#define MAX_WINDOWS 16
static HWND windows[MAX_WINDOWS];

static void register_window(HWND hwnd)
{
    RECT rect = {0}, window = {0};
    POINT origin = {0, 0};
    UINT drawable;
    unsigned int i;

    if (!hwnd)
        return;
    /* winex11 keeps the window's X window in this property (dlls/winex11.drv/window.c whole_window_prop) */
    drawable = (UINT)(ULONG_PTR)GetPropA(hwnd, "__wine_x11_whole_window");
    GetClientRect(hwnd, &rect);
    ClientToScreen(hwnd, &origin);
    GetWindowRect(hwnd, &window);
    wd3d_register_window_call(hwnd, drawable, origin.x, origin.y, rect.right - rect.left, rect.bottom - rect.top,
            IsWindowVisible(hwnd) ? 1 : 0, origin.x - window.left, origin.y - window.top,
            window.right - window.left, window.bottom - window.top);
    for (i = 0; i < MAX_WINDOWS; ++i)
    {
        if (windows[i] == hwnd)
            return;
        if (!windows[i])
        {
            windows[i] = hwnd;
            return;
        }
    }
}

void __cdecl wd3d_window_arg(HWND hwnd)
{
    register_window(hwnd);
}

/* struct wined3d_swapchain_desc: device_window at offset 36 */
void __cdecl wd3d_window_desc(const void *desc)
{
    if (desc)
        register_window(*(HWND *)((const char *)desc + 36));
}

void __cdecl wd3d_refresh_windows(void)
{
    unsigned int i;

    for (i = 0; i < MAX_WINDOWS && windows[i]; ++i)
    {
        if (IsWindow(windows[i]))
            register_window(windows[i]);
    }
}

/* struct wined3d_output_desc (wined3d.h) */
struct output_desc
{
    UINT ordinal;
    WCHAR device_name[CCHDEVICENAME];
    RECT desktop_rect;
    BOOL attached_to_desktop;
    HMONITOR monitor;
};

static BOOL CALLBACK find_monitor(HMONITOR monitor, HDC dc, RECT *rect, LPARAM lparam)
{
    struct output_desc *desc = (struct output_desc *)lparam;
    MONITORINFOEXW info;
    unsigned int i;

    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, (MONITORINFO *)&info))
        return TRUE;
    for (i = 0; i < CCHDEVICENAME && info.szDevice[i] == desc->device_name[i] && info.szDevice[i]; ++i);
    if (i < CCHDEVICENAME && info.szDevice[i] != desc->device_name[i])
        return TRUE;
    desc->monitor = monitor;
    return FALSE;
}

/* wined3d_output_get_desc: the host's HMONITOR means nothing here; take the guest monitor of the same device
 * (dxgi and d3d9 compare it with MonitorFromWindow / hand it to the game). */
HRESULT __cdecl wd3d_output_desc_post(struct output_desc *desc, HRESULT hr)
{
    POINT pt;

    if (FAILED(hr) || !desc)
        return hr;
    desc->monitor = NULL;
    EnumDisplayMonitors(NULL, NULL, find_monitor, (LPARAM)desc);
    if (!desc->monitor)
    {
        pt.x = desc->desktop_rect.left;
        pt.y = desc->desktop_rect.top;
        desc->monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    }
    return hr;
}

/* ---- GDI over texture memory ---- */

/* The guest DC made for each host DC wined3d handed out. A few at a time (each lives from get_dc to release_dc
 * for d3d11/dxgi). */
#define MAX_DCS 64
static struct
{
    UINT host;
    HDC dc;
    HANDLE bitmap;
} dcs[MAX_DCS];

/* wined3d_texture_get_dc(texture, sub_resource_idx, HDC *dc): *dc is the host's DC; give the guest one over the
 * same memory instead. */
HRESULT __cdecl wd3d_get_dc_post(UINT *args, HRESULT hr)
{
    HDC *out = (HDC *)args[2];
    D3DKMT_CREATEDCFROMMEMORY desc = {0};
    UINT host, info[5];
    unsigned int i, free_slot = MAX_DCS;

    if (FAILED(hr) || !out)
        return hr;
    host = (UINT)(ULONG_PTR)*out;
    for (i = 0; i < MAX_DCS; ++i)
    {
        if (dcs[i].host == host && dcs[i].dc)
        {
            *out = dcs[i].dc; /* an OWNDC texture's DC lives across get_dc / release_dc */
            return hr;
        }
        if (!dcs[i].dc && free_slot == MAX_DCS)
            free_slot = i;
    }
    if (free_slot < MAX_DCS && wd3d_dc_info_call(host, info))
    {
        desc.pMemory = (void *)info[0];
        desc.Format = info[1];
        desc.Width = info[2];
        desc.Height = info[3];
        desc.Pitch = info[4];
        desc.hDeviceDc = CreateCompatibleDC(NULL);
        if (desc.hDeviceDc && !D3DKMTCreateDCFromMemory(&desc))
        {
            DeleteDC(desc.hDeviceDc);
            dcs[free_slot].host = host;
            dcs[free_slot].dc = desc.hDc;
            dcs[free_slot].bitmap = desc.hBitmap;
            *out = desc.hDc;
            return hr;
        }
        if (desc.hDeviceDc)
            DeleteDC(desc.hDeviceDc);
    }
    /* no guest DC: give the host one back (release_dc passes a DC it doesn't know through as is) */
    wined3d_texture_release_dc((void *)args[0], args[1], (HDC)(ULONG_PTR)host);
    *out = NULL;
    return 0x8876086c; /* WINED3DERR_INVALIDCALL */
}

/* wined3d_texture_release_dc's HDC argument (in place): the guest's DC back to the host's */
void __cdecl wd3d_dc_arg(HDC *dc)
{
    unsigned int i;

    for (i = 0; i < MAX_DCS; ++i)
        if (dcs[i].dc && dcs[i].dc == *dc)
        {
            *dc = (HDC)(ULONG_PTR)dcs[i].host;
            return;
        }
}

static void dc_destroyed(UINT host_dc)
{
    D3DKMT_DESTROYDCFROMMEMORY desc;
    unsigned int i;

    for (i = 0; i < MAX_DCS; ++i)
        if (dcs[i].dc && dcs[i].host == host_dc)
        {
            desc.hDc = dcs[i].dc;
            desc.hBitmap = dcs[i].bitmap;
            D3DKMTDestroyDCFromMemory(&desc);
            dcs[i].dc = NULL;
            dcs[i].host = 0;
            return;
        }
}

/* vkd3d-shader's signature API (vkd3d_shader.h), which wined3d.dll exports and d3d11's CreateInputLayout uses:
 * the input signature (ISGN/ISG1 chunk) of a DXBC blob, parsed as libs/vkd3d-shader/dxbc.c does (without the
 * checksum check). One allocation: the elements, then their names. */
struct vkd3d_shader_code
{
    const void *code;
    size_t size;
};

struct vkd3d_shader_signature_element
{
    const char *semantic_name;
    unsigned int semantic_index, stream_index, sysval_semantic, component_type, register_index, mask, used_mask;
    unsigned int min_precision;
};

struct vkd3d_shader_signature
{
    struct vkd3d_shader_signature_element *elements;
    unsigned int element_count;
};

void __cdecl vkd3d_shader_free_shader_signature(struct vkd3d_shader_signature *signature);

#define VKD3D_ERROR_OUT_OF_MEMORY (-2)
#define VKD3D_ERROR_INVALID_ARGUMENT (-3)
#define DXBC_TAG(a, b, c, d) ((UINT)(a) | (UINT)(b) << 8 | (UINT)(c) << 16 | (UINT)(d) << 24)

static UINT read_u32(const BYTE *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (UINT)p[3] << 24;
}

/* length of the string at offset in [0, size), or -1 if it isn't terminated in it */
static int string_length(const BYTE *data, UINT size, UINT offset)
{
    UINT i;

    for (i = offset; i < size; ++i)
        if (!data[i])
            return i - offset;
    return -1;
}

static int parse_signature_chunk(const BYTE *data, UINT size, int min_precision,
        struct vkd3d_shader_signature *signature)
{
    UINT count, header_size, stride, names_size = 0, i;
    struct vkd3d_shader_signature_element *e;
    const BYTE *p;
    char *names;
    int len;

    if (size < 8)
        return VKD3D_ERROR_INVALID_ARGUMENT;
    count = read_u32(data);
    header_size = read_u32(data + 4);
    stride = min_precision ? 32 : 24;
    if ((header_size & 3) || header_size < 8 || header_size > size || count > (size - header_size) / stride)
        return VKD3D_ERROR_INVALID_ARGUMENT;
    for (i = 0, p = data + header_size; i < count; ++i, p += stride)
    {
        if ((len = string_length(data, size, read_u32(p + (min_precision ? 4 : 0)))) < 0)
            return VKD3D_ERROR_INVALID_ARGUMENT;
        names_size += len + 1;
    }
    if (!count)
        return 0;
    if (!(e = HeapAlloc(GetProcessHeap(), 0, count * sizeof(*e) + names_size)))
        return VKD3D_ERROR_OUT_OF_MEMORY;
    names = (char *)(e + count);
    for (i = 0, p = data + header_size; i < count; ++i, p += stride)
    {
        const BYTE *q = p;
        const char *name;

        e[i].stream_index = min_precision ? read_u32(q) : 0;
        if (min_precision)
            q += 4;
        name = (const char *)data + read_u32(q);
        e[i].semantic_name = names;
        while ((*names++ = *name++));
        e[i].semantic_index = read_u32(q + 4);
        e[i].sysval_semantic = read_u32(q + 8);
        e[i].component_type = read_u32(q + 12);
        e[i].register_index = read_u32(q + 16);
        e[i].mask = q[20];
        e[i].used_mask = q[21];
        e[i].min_precision = min_precision ? read_u32(q + 24) : 0;
        if (e[i].component_type > 3) /* VKD3D_SHADER_COMPONENT_FLOAT */
        {
            HeapFree(GetProcessHeap(), 0, e);
            return VKD3D_ERROR_INVALID_ARGUMENT;
        }
    }
    signature->elements = e;
    signature->element_count = count;
    return 0;
}

int __cdecl vkd3d_shader_parse_input_signature(const struct vkd3d_shader_code *dxbc,
        struct vkd3d_shader_signature *signature, char **messages)
{
    const BYTE *data = dxbc->code;
    UINT size = dxbc->size, count, i, offset, tag, chunk_size;
    int ret = 0, found = 0;

    if (messages)
        *messages = NULL;
    signature->elements = NULL;
    signature->element_count = 0;
    if (size < 32 || read_u32(data) != DXBC_TAG('D','X','B','C') || read_u32(data + 20) != 1)
        return VKD3D_ERROR_INVALID_ARGUMENT;
    count = read_u32(data + 28);
    if (count > (size - 32) / 4)
        return VKD3D_ERROR_INVALID_ARGUMENT;
    for (i = 0; i < count; ++i)
    {
        offset = read_u32(data + 32 + 4 * i);
        if (offset > size - 8)
            return VKD3D_ERROR_INVALID_ARGUMENT;
        tag = read_u32(data + offset);
        chunk_size = read_u32(data + offset + 4);
        if (chunk_size > size - offset - 8)
            return VKD3D_ERROR_INVALID_ARGUMENT;
        if (tag != DXBC_TAG('I','S','G','N') && tag != DXBC_TAG('I','S','G','1'))
            continue;
        if (found) /* vkd3d: FIXME multiple input signatures, the last one wins */
            vkd3d_shader_free_shader_signature(signature);
        found = 1;
        if ((ret = parse_signature_chunk(data + offset + 8, chunk_size, tag == DXBC_TAG('I','S','G','1'), signature)))
            return ret;
    }
    return 0;
}

void __cdecl vkd3d_shader_free_shader_signature(struct vkd3d_shader_signature *signature)
{
    HeapFree(GetProcessHeap(), 0, signature->elements);
    signature->elements = NULL;
}

/* vkd3d-shader's DXBC container API (vkd3d_shader.h), which d3d10 imports from wined3d.dll (effects): parse,
 * serialize and free, as libs/vkd3d-shader/dxbc.c does them. The checksum is DXBC's MD5 variant (checksum.c:
 * MD5 by Colin Plumb, public domain; only the final padding differs). */
struct vkd3d_shader_dxbc_section_desc
{
    UINT tag;
    struct vkd3d_shader_code data;
};

struct vkd3d_shader_dxbc_desc
{
    UINT tag;
    UINT checksum[4];
    unsigned int version;
    size_t size;
    size_t section_count;
    struct vkd3d_shader_dxbc_section_desc *sections;
};

#define VKD3D_SHADER_PARSE_DXBC_IGNORE_CHECKSUM 1

#define F1(x, y, z) (z ^ (x & (y ^ z)))
#define F2(x, y, z) F1(z, x, y)
#define F3(x, y, z) (x ^ y ^ z)
#define F4(x, y, z) (y ^ (x | ~z))
#define MD5STEP(f, w, x, y, z, data, s) (w += f(x, y, z) + data, w = w << s | w >> (32 - s), w += x)

static void md5_transform(UINT buf[4], const UINT in[16])
{
    UINT a = buf[0], b = buf[1], c = buf[2], d = buf[3];

    MD5STEP(F1, a, b, c, d, in[0] + 0xd76aa478, 7);
    MD5STEP(F1, d, a, b, c, in[1] + 0xe8c7b756, 12);
    MD5STEP(F1, c, d, a, b, in[2] + 0x242070db, 17);
    MD5STEP(F1, b, c, d, a, in[3] + 0xc1bdceee, 22);
    MD5STEP(F1, a, b, c, d, in[4] + 0xf57c0faf, 7);
    MD5STEP(F1, d, a, b, c, in[5] + 0x4787c62a, 12);
    MD5STEP(F1, c, d, a, b, in[6] + 0xa8304613, 17);
    MD5STEP(F1, b, c, d, a, in[7] + 0xfd469501, 22);
    MD5STEP(F1, a, b, c, d, in[8] + 0x698098d8, 7);
    MD5STEP(F1, d, a, b, c, in[9] + 0x8b44f7af, 12);
    MD5STEP(F1, c, d, a, b, in[10] + 0xffff5bb1, 17);
    MD5STEP(F1, b, c, d, a, in[11] + 0x895cd7be, 22);
    MD5STEP(F1, a, b, c, d, in[12] + 0x6b901122, 7);
    MD5STEP(F1, d, a, b, c, in[13] + 0xfd987193, 12);
    MD5STEP(F1, c, d, a, b, in[14] + 0xa679438e, 17);
    MD5STEP(F1, b, c, d, a, in[15] + 0x49b40821, 22);

    MD5STEP(F2, a, b, c, d, in[1] + 0xf61e2562, 5);
    MD5STEP(F2, d, a, b, c, in[6] + 0xc040b340, 9);
    MD5STEP(F2, c, d, a, b, in[11] + 0x265e5a51, 14);
    MD5STEP(F2, b, c, d, a, in[0] + 0xe9b6c7aa, 20);
    MD5STEP(F2, a, b, c, d, in[5] + 0xd62f105d, 5);
    MD5STEP(F2, d, a, b, c, in[10] + 0x02441453, 9);
    MD5STEP(F2, c, d, a, b, in[15] + 0xd8a1e681, 14);
    MD5STEP(F2, b, c, d, a, in[4] + 0xe7d3fbc8, 20);
    MD5STEP(F2, a, b, c, d, in[9] + 0x21e1cde6, 5);
    MD5STEP(F2, d, a, b, c, in[14] + 0xc33707d6, 9);
    MD5STEP(F2, c, d, a, b, in[3] + 0xf4d50d87, 14);
    MD5STEP(F2, b, c, d, a, in[8] + 0x455a14ed, 20);
    MD5STEP(F2, a, b, c, d, in[13] + 0xa9e3e905, 5);
    MD5STEP(F2, d, a, b, c, in[2] + 0xfcefa3f8, 9);
    MD5STEP(F2, c, d, a, b, in[7] + 0x676f02d9, 14);
    MD5STEP(F2, b, c, d, a, in[12] + 0x8d2a4c8a, 20);

    MD5STEP(F3, a, b, c, d, in[5] + 0xfffa3942, 4);
    MD5STEP(F3, d, a, b, c, in[8] + 0x8771f681, 11);
    MD5STEP(F3, c, d, a, b, in[11] + 0x6d9d6122, 16);
    MD5STEP(F3, b, c, d, a, in[14] + 0xfde5380c, 23);
    MD5STEP(F3, a, b, c, d, in[1] + 0xa4beea44, 4);
    MD5STEP(F3, d, a, b, c, in[4] + 0x4bdecfa9, 11);
    MD5STEP(F3, c, d, a, b, in[7] + 0xf6bb4b60, 16);
    MD5STEP(F3, b, c, d, a, in[10] + 0xbebfbc70, 23);
    MD5STEP(F3, a, b, c, d, in[13] + 0x289b7ec6, 4);
    MD5STEP(F3, d, a, b, c, in[0] + 0xeaa127fa, 11);
    MD5STEP(F3, c, d, a, b, in[3] + 0xd4ef3085, 16);
    MD5STEP(F3, b, c, d, a, in[6] + 0x04881d05, 23);
    MD5STEP(F3, a, b, c, d, in[9] + 0xd9d4d039, 4);
    MD5STEP(F3, d, a, b, c, in[12] + 0xe6db99e5, 11);
    MD5STEP(F3, c, d, a, b, in[15] + 0x1fa27cf8, 16);
    MD5STEP(F3, b, c, d, a, in[2] + 0xc4ac5665, 23);

    MD5STEP(F4, a, b, c, d, in[0] + 0xf4292244, 6);
    MD5STEP(F4, d, a, b, c, in[7] + 0x432aff97, 10);
    MD5STEP(F4, c, d, a, b, in[14] + 0xab9423a7, 15);
    MD5STEP(F4, b, c, d, a, in[5] + 0xfc93a039, 21);
    MD5STEP(F4, a, b, c, d, in[12] + 0x655b59c3, 6);
    MD5STEP(F4, d, a, b, c, in[3] + 0x8f0ccc92, 10);
    MD5STEP(F4, c, d, a, b, in[10] + 0xffeff47d, 15);
    MD5STEP(F4, b, c, d, a, in[1] + 0x85845dd1, 21);
    MD5STEP(F4, a, b, c, d, in[8] + 0x6fa87e4f, 6);
    MD5STEP(F4, d, a, b, c, in[15] + 0xfe2ce6e0, 10);
    MD5STEP(F4, c, d, a, b, in[6] + 0xa3014314, 15);
    MD5STEP(F4, b, c, d, a, in[13] + 0x4e0811a1, 21);
    MD5STEP(F4, a, b, c, d, in[4] + 0xf7537e82, 6);
    MD5STEP(F4, d, a, b, c, in[11] + 0xbd3af235, 10);
    MD5STEP(F4, c, d, a, b, in[2] + 0x2ad7d2bb, 15);
    MD5STEP(F4, b, c, d, a, in[9] + 0xeb86d391, 21);

    buf[0] += a;
    buf[1] += b;
    buf[2] += c;
    buf[3] += d;
}

static void copy_bytes(void *dst, const void *src, size_t size)
{
    BYTE *d = dst;
    const BYTE *s = src;

    while (size--)
        *d++ = *s++;
}

static void fill_bytes(void *dst, BYTE value, size_t size)
{
    BYTE *d = dst;

    while (size--)
        *d++ = value;
}

/* checksum of a DXBC blob: everything after the tag and the checksum itself (20 bytes), size > 20 */
static void dxbc_checksum(const BYTE *data, size_t size, UINT checksum[4])
{
    UINT buf[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476}, in[16], bits, i;
    size_t rest;

    data += 20;
    size -= 20;
    bits = (UINT)size << 3;
    for (; size >= 64; data += 64, size -= 64)
    {
        copy_bytes(in, data, 64);
        md5_transform(buf, in);
    }
    /* DXBC's padding: 0x80 after the data; the bit count goes first in the last block (4 bytes before the data)
     * when the tail fits in it, and bits >> 2 | 1 at its end */
    rest = size;
    fill_bytes(in, 0, sizeof(in));
    if (rest + 1 > 56)
    {
        copy_bytes(in, data, rest);
        ((BYTE *)in)[rest] = 0x80;
        md5_transform(buf, in);
        fill_bytes(in, 0, sizeof(in));
    }
    else
    {
        copy_bytes((BYTE *)in + 4, data, rest);
        ((BYTE *)in)[rest + 4] = 0x80;
    }
    in[0] = bits;
    in[15] = bits >> 2 | 1;
    md5_transform(buf, in);
    for (i = 0; i < 4; ++i)
        checksum[i] = buf[i];
}

int __cdecl vkd3d_shader_parse_dxbc(const struct vkd3d_shader_code *dxbc, UINT flags,
        struct vkd3d_shader_dxbc_desc *desc, char **messages)
{
    struct vkd3d_shader_dxbc_section_desc *sections = NULL;
    const BYTE *data = dxbc->code;
    size_t size = dxbc->size;
    UINT checksum[4], count, offset, chunk_size, i;

    if (messages)
        *messages = NULL;
    if (size < 32 || read_u32(data) != DXBC_TAG('D','X','B','C'))
        return VKD3D_ERROR_INVALID_ARGUMENT;
    if (!(flags & VKD3D_SHADER_PARSE_DXBC_IGNORE_CHECKSUM))
    {
        dxbc_checksum(data, size, checksum);
        for (i = 0; i < 4; ++i)
            if (checksum[i] != read_u32(data + 4 + 4 * i))
                return VKD3D_ERROR_INVALID_ARGUMENT;
    }
    if (read_u32(data + 20) != 1)
        return VKD3D_ERROR_INVALID_ARGUMENT;
    count = read_u32(data + 28);
    if (count > (size - 32) / 4)
        return VKD3D_ERROR_INVALID_ARGUMENT;
    if (count && !(sections = HeapAlloc(GetProcessHeap(), 0, count * sizeof(*sections))))
        return VKD3D_ERROR_OUT_OF_MEMORY;
    for (i = 0; i < count; ++i)
    {
        offset = read_u32(data + 32 + 4 * i);
        if (offset >= size || size - offset < 8 || (chunk_size = read_u32(data + offset + 4)) > size - offset - 8)
        {
            HeapFree(GetProcessHeap(), 0, sections);
            return VKD3D_ERROR_INVALID_ARGUMENT;
        }
        sections[i].tag = read_u32(data + offset);
        sections[i].data.code = data + offset + 8;
        sections[i].data.size = chunk_size;
    }
    desc->tag = DXBC_TAG('D','X','B','C');
    for (i = 0; i < 4; ++i)
        desc->checksum[i] = read_u32(data + 4 + 4 * i);
    desc->version = 1;
    desc->size = read_u32(data + 24);
    desc->section_count = count;
    desc->sections = sections;
    return 0;
}

void __cdecl vkd3d_shader_free_dxbc(struct vkd3d_shader_dxbc_desc *dxbc)
{
    HeapFree(GetProcessHeap(), 0, dxbc->sections);
}

static void write_u32(BYTE *p, UINT v)
{
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

/* sections start 4-aligned (padding 0xab, as vkd3d's bytecode buffer); the blob ends right after the last one */
int __cdecl vkd3d_shader_serialize_dxbc(size_t section_count, const struct vkd3d_shader_dxbc_section_desc *sections,
        struct vkd3d_shader_code *dxbc, char **messages)
{
    size_t size = 32 + 4 * section_count, i;
    UINT checksum[4];
    BYTE *data;

    if (messages)
        *messages = NULL;
    for (i = 0; i < section_count; ++i)
        size = ((size + 3) & ~(size_t)3) + 8 + sections[i].data.size;
    if (!(data = HeapAlloc(GetProcessHeap(), 0, size)))
        return VKD3D_ERROR_OUT_OF_MEMORY;
    write_u32(data, DXBC_TAG('D','X','B','C'));
    write_u32(data + 20, 1);
    write_u32(data + 24, size);
    write_u32(data + 28, section_count);
    size = 32 + 4 * section_count;
    for (i = 0; i < section_count; ++i)
    {
        fill_bytes(data + size, 0xab, ((size + 3) & ~(size_t)3) - size);
        size = (size + 3) & ~(size_t)3;
        write_u32(data + 32 + 4 * i, size);
        write_u32(data + size, sections[i].tag);
        write_u32(data + size + 4, sections[i].data.size);
        copy_bytes(data + size + 8, sections[i].data.code, sections[i].data.size);
        size += 8 + sections[i].data.size;
    }
    dxbc_checksum(data, size, checksum);
    for (i = 0; i < 4; ++i)
        write_u32(data + 4 + 4 * i, checksum[i]);
    dxbc->code = data;
    dxbc->size = size;
    return 0;
}

void __cdecl vkd3d_shader_free_shader_code(struct vkd3d_shader_code *code)
{
    HeapFree(GetProcessHeap(), 0, (void *)code->code);
}
