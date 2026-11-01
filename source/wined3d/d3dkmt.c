/* Host wined3d (HOST_WINED3D=1): the D3DKMT (kernel-mode thunk) calls wined3d makes, with fixed answers: one adapter,
 * one device, no exclusive-mode ownership to manage, no present-rect escape. */
#include <stdarg.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "ddk/d3dkmthk.h"
#include "wd3d_shim.h"
#include "hostwd3d_bridge.h"

#define WD3D_KMT_ADAPTER 1
#define WD3D_KMT_DEVICE 2
static const LUID adapter_luid = { 0x5d3d, 0 };

NTSTATUS WINAPI D3DKMTOpenAdapterFromGdiDisplayName(D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME *desc)
{
    desc->hAdapter = WD3D_KMT_ADAPTER;
    desc->AdapterLuid = adapter_luid;
    desc->VidPnSourceId = 0;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI D3DKMTOpenAdapterFromLuid(D3DKMT_OPENADAPTERFROMLUID *desc)
{
    desc->hAdapter = WD3D_KMT_ADAPTER;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI D3DKMTCloseAdapter(const D3DKMT_CLOSEADAPTER *desc)
{
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI D3DKMTCreateDevice(D3DKMT_CREATEDEVICE *desc)
{
    desc->hDevice = WD3D_KMT_DEVICE;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI D3DKMTDestroyDevice(const D3DKMT_DESTROYDEVICE *desc)
{
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI D3DKMTSetVidPnSourceOwner(const D3DKMT_SETVIDPNSOURCEOWNER *desc)
{
    return STATUS_SUCCESS;
}

/* Not supported: wined3d then uses its own video memory estimate (directx.c). */
NTSTATUS WINAPI D3DKMTQueryVideoMemoryInfo(D3DKMT_QUERYVIDEOMEMORYINFO *desc)
{
    return STATUS_NOT_SUPPORTED;
}

/* D3DKMT_ESCAPE_SET_PRESENT_RECT_WINE (child-window presents): not supported. */
NTSTATUS WINAPI D3DKMTEscape(const D3DKMT_ESCAPE *desc)
{
    return STATUS_NOT_IMPLEMENTED;
}

/* A GDI DC over a texture's memory (GetDC on surfaces, ddraw and d3d9). shortcut: unsupported until the GDI bridge
 * covers memory DCs (phase 3, ddraw/d3d9 games); wined3d reports the failure to the caller. */
/* wined3d gives some textures a DC over their memory (the no-3D front buffer; GetDC on surfaces). shortcut: a memory
 * DC of the window/DC table, not backed by the texture's memory: GDI drawing into it is lost (texture get_dc/release_dc
 * aren't forwarded yet, so no guest reaches it). */
NTSTATUS WINAPI D3DKMTCreateDCFromMemory(D3DKMT_CREATEDCFROMMEMORY *desc)
{
    uint32_t dc = hostwd3d_dc_create_memory();

    if (!dc)
        return STATUS_NO_MEMORY;
    desc->hDc = (HDC)(ULONG_PTR)dc;
    desc->hBitmap = (HANDLE)(ULONG_PTR)dc; /* only handed back to D3DKMTDestroyDCFromMemory */
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI D3DKMTDestroyDCFromMemory(const D3DKMT_DESTROYDCFROMMEMORY *desc)
{
    return hostwd3d_dc_delete((uint32_t)(ULONG_PTR)desc->hDc) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}
