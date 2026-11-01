/* Host wined3d (HOST_WINED3D=1): registry, module handles and GetProcAddress.
 * wined3d reads its settings from HKCU\Software\Wine\Direct3D at DLL_PROCESS_ATTACH; here that key is a small
 * table set from host code (wd3d_config_set_*). Defaults: csmt=0, since the host has one thread for it, and the
 * WebGL profile the guest's wined3d gets (WINE_D3D_CONFIG=webgl=1,webgl_glsl_es=1, source/sdl/startupArgs.cpp). */
#include <stdarg.h>
#include <string.h>
#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "wd3d_shim.h"

/* Registry */
#define WD3D_REG_DIRECT3D ((HKEY)0x10000020)
#define WD3D_CONFIG_MAX 32

static struct
{
    char name[32];
    DWORD type;
    char value[64]; /* REG_SZ: the string; REG_DWORD: the DWORD in the first 4 bytes */
} config[WD3D_CONFIG_MAX] =
{
    { "csmt", REG_DWORD, { 0 } },
    { "webgl", REG_DWORD, { 1 } },
    { "webgl_glsl_es", REG_DWORD, { 1 } },
};

static int config_find(const char *name, BOOL add)
{
    int i, free_slot = -1;

    for (i = 0; i < WD3D_CONFIG_MAX; i++)
    {
        if (!config[i].name[0]) { if (free_slot < 0) free_slot = i; continue; }
        if (!strcmp(config[i].name, name)) return i;
    }
    if (!add || free_slot < 0 || strlen(name) >= sizeof(config[0].name)) return -1;
    strcpy(config[free_slot].name, name);
    return free_slot;
}

void wd3d_config_set_dword(const char *name, DWORD value)
{
    int i;

    if ((i = config_find(name, TRUE)) < 0) return;
    config[i].type = REG_DWORD;
    memcpy(config[i].value, &value, sizeof(value));
}

void wd3d_config_set_string(const char *name, const char *value)
{
    int i;

    if (!value)
    {
        if ((i = config_find(name, FALSE)) >= 0) config[i].name[0] = 0;
        return;
    }
    if (strlen(value) >= sizeof(config[0].value) || (i = config_find(name, TRUE)) < 0) return;
    config[i].type = REG_SZ;
    strcpy(config[i].value, value);
}

LSTATUS WINAPI RegOpenKeyA(HKEY parent, const char *name, HKEY *key)
{
    if (parent == HKEY_CURRENT_USER && name && !strcmp(name, "Software\\Wine\\Direct3D"))
    {
        *key = WD3D_REG_DIRECT3D;
        return ERROR_SUCCESS;
    }
    *key = 0;
    return ERROR_FILE_NOT_FOUND; /* no per-application keys */
}

LSTATUS WINAPI RegQueryValueExA(HKEY key, const char *name, DWORD *reserved, DWORD *type, BYTE *data, DWORD *count)
{
    DWORD size;
    int i;

    if (key != WD3D_REG_DIRECT3D) return ERROR_INVALID_HANDLE;
    if (!name || (i = config_find(name, FALSE)) < 0) return ERROR_FILE_NOT_FOUND;
    size = config[i].type == REG_DWORD ? sizeof(DWORD) : strlen(config[i].value) + 1;
    if (type) *type = config[i].type;
    if (data)
    {
        if (!count || *count < size)
        {
            if (count) *count = size;
            return ERROR_MORE_DATA;
        }
        memcpy(data, config[i].value, size);
    }
    if (count) *count = size;
    return ERROR_SUCCESS;
}

LSTATUS WINAPI RegCloseKey(HKEY key)
{
    return key == WD3D_REG_DIRECT3D ? ERROR_SUCCESS : ERROR_INVALID_HANDLE;
}

/* Modules: wined3d itself, plus the two it asks for by name. */
#define WD3D_OPENGL32 ((HMODULE)0x10000030)
#define WD3D_NTDLL ((HMODULE)0x10000040)

static HMODULE module_by_name(const char *name)
{
    if (!name) return NULL; /* the exe: there is none on the host */
    if (!_stricmp(name, "opengl32.dll") || !_stricmp(name, "opengl32")) return WD3D_OPENGL32;
    if (!_stricmp(name, "ntdll.dll") || !_stricmp(name, "ntdll")) return WD3D_NTDLL;
    if (!_stricmp(name, "wined3d.dll") || !_stricmp(name, "wined3d")) return WD3D_MODULE;
    return NULL;
}

HMODULE WINAPI GetModuleHandleA(const char *name)
{
    HMODULE module = module_by_name(name);

    if (!module) SetLastError(ERROR_MOD_NOT_FOUND);
    return module;
}

HMODULE WINAPI GetModuleHandleW(const WCHAR *name)
{
    char ascii[MAX_PATH];
    int i;

    if (!name) return GetModuleHandleA(NULL);
    for (i = 0; name[i] && i < MAX_PATH - 1; i++) ascii[i] = name[i] < 0x80 ? name[i] : '?';
    ascii[i] = 0;
    return GetModuleHandleA(ascii);
}

/* Only asked FROM_ADDRESS for wined3d's own code (cs.c pins itself, view.c): that is wined3d. */
BOOL WINAPI GetModuleHandleExW(DWORD flags, const WCHAR *name, HMODULE *module)
{
    *module = flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS ? WD3D_MODULE : GetModuleHandleW(name);
    return *module != NULL;
}

/* No executable on the host, so no HKCU\Software\Wine\AppDefaults\<app> settings. */
DWORD WINAPI GetModuleFileNameA(HMODULE module, char *name, DWORD size)
{
    SetLastError(ERROR_MOD_NOT_FOUND);
    return 0;
}

FARPROC WINAPI GetProcAddress(HMODULE module, const char *name)
{
    FARPROC proc = NULL;

    /* GL 1.1 entry points and wglSwapBuffers/wglGetPixelFormat (adapter_gl.c): the host GL bridge. */
    /* Names only, no ordinals: HIWORD(name) can't tell them apart here, wasm's static strings start below 64 KB
     * (glGetIntegerv's name did, and wined3d got a NULL glGetIntegerv). */
    if (module == WD3D_OPENGL32 && name) proc = (FARPROC)wd3d_opengl32_proc(name);
    /* ntdll's NtAlertThreadByThreadId/NtWaitForAlertByThreadId are for the command stream thread: absent. */
    if (!proc) SetLastError(ERROR_PROC_NOT_FOUND);
    return proc;
}

BOOL WINAPI FreeLibrary(HMODULE module)
{
    return TRUE;
}
