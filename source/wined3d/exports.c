/* Host wined3d (HOST_WINED3D=1): every wined3d_* export by name, the table the guest thunk will dispatch through
 * (phase 2). Exporting wd3d_link_root keeps it, and so all of wined3d, in the link: nothing calls it yet. */
#define COBJMACROS
#include <stdarg.h>
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "objbase.h"
#include "wine/wined3d.h"
#include "wd3d_shim.h"

#define X(name) { #name, (void *)name },
static const struct wd3d_export wd3d_exports[] =
{
#include "wined3d_exports.inc"
};
#undef X

const struct wd3d_export * __cdecl wd3d_link_root(unsigned int *count)
{
    *count = ARRAY_SIZE(wd3d_exports);
    return wd3d_exports;
}

/* wined3d_main.c's DllMain: DLL_PROCESS_ATTACH reads the settings (module.c's registry) and sets up TLS and the
 * window class; it has to run before any export. The module handle is WD3D_MODULE. */
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved);

BOOL __cdecl wd3d_dll_main(DWORD reason)
{
    return DllMain(WD3D_MODULE, reason, NULL);
}
