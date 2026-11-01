/* Host wined3d shims (HOST_WINED3D=1): what the C shims share. Compiled against Wine's headers, so Win32 types are
 * Wine's (32-bit layout, 2-byte WCHAR). */
#ifndef WD3D_SHIM_H
#define WD3D_SHIM_H

struct wd3d_export
{
    const char *name;
    void *fn;
};
const struct wd3d_export * __cdecl wd3d_link_root(unsigned int *count);
/* exports.c: wined3d's DllMain for this module; DLL_PROCESS_ATTACH before any export, DLL_PROCESS_DETACH at the end. */
BOOL __cdecl wd3d_dll_main(DWORD reason);
/* The HMODULE the shims report for wined3d (GetModuleHandle*, resources). */
#define WD3D_MODULE ((HINSTANCE)0x10000000)

/* log.cpp: one line to the Boxedwine log (klog). */
void wd3d_log(const char *text);

/* module.c: the values wined3d reads under HKCU\Software\Wine\Direct3D at DLL_PROCESS_ATTACH (csmt=0 by default).
 * wd3d_config_set_string(name, NULL) removes a setting. */
void wd3d_config_set_dword(const char *name, DWORD value);
void wd3d_config_set_string(const char *name, const char *value);

/* The host GL bridge (phase 1-B): opengl32.dll's export by name, what GetProcAddress on opengl32 returns. */
void *wd3d_opengl32_proc(const char *name);

#endif
