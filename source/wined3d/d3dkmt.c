/* Host wined3d (HOST_WINED3D=1): the D3DKMT (kernel-mode thunk) calls wined3d makes, with fixed answers: one adapter,
 * one device, no exclusive-mode ownership to manage, no present-rect escape. */
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "ddk/d3dkmthk.h"
#include "wd3d_shim.h"
#include "hostwd3d_bridge.h"
#include "hostwd3d_callbacks.h"

/* thunkHost.cpp */
uint32_t wd3d_map_native(void *host, uint32_t len);
void wd3d_unmap_native(uint32_t guest, uint32_t len);
const void *wd3d_call_memory(void);

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

/* A GDI DC over a texture's memory (wined3d_texture_get_dc; OWNDC textures; the no-3D front buffer). The host DC is
 * a memory DC of the window/DC table that only names the record below: GDI runs in the guest. The guest stub asks for
 * the record with id 3 (hostwd3d_dc_info: the memory is mapped into the guest then) and makes its own DC over the same
 * bytes with the guest's D3DKMTCreateDCFromMemory; DC_DESTROYED tells it when wined3d drops the host DC. */
struct dc_record
{
    uint32_t dc;
    void *memory;
    uint32_t guest;        /* the memory mapped into the guest (id 3), 0 = not yet */
    const void *space;     /* the guest address space it is mapped in (wd3d_call_memory) */
    uint32_t size;
    D3DDDIFORMAT format;
    uint32_t width, height, pitch;
};

static struct dc_record *records;
static unsigned int record_count, record_size;

static struct dc_record *find_record(uint32_t dc)
{
    unsigned int i;

    for (i = 0; i < record_count; ++i)
        if (records[i].dc == dc)
            return &records[i];
    return NULL;
}

NTSTATUS WINAPI D3DKMTCreateDCFromMemory(D3DKMT_CREATEDCFROMMEMORY *desc)
{
    struct dc_record *r;
    uint32_t dc;

    if (!desc->pMemory || !desc->Pitch || !desc->Height || desc->Height > UINT_MAX / desc->Pitch)
        return STATUS_INVALID_PARAMETER;
    if (record_count == record_size)
    {
        unsigned int n = record_size ? record_size * 2 : 16;
        struct dc_record *grown = realloc(records, n * sizeof(*records));

        if (!grown)
            return STATUS_NO_MEMORY;
        records = grown;
        record_size = n;
    }
    if (!(dc = hostwd3d_dc_create_memory()))
        return STATUS_NO_MEMORY;
    r = &records[record_count++];
    r->dc = dc;
    r->memory = desc->pMemory;
    r->guest = 0;
    r->size = desc->Pitch * desc->Height;
    r->format = desc->Format;
    r->width = desc->Width;
    r->height = desc->Height;
    r->pitch = desc->Pitch;
    desc->hDc = (HDC)(ULONG_PTR)dc;
    desc->hBitmap = (HANDLE)(ULONG_PTR)dc; /* only handed back to D3DKMTDestroyDCFromMemory */
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI D3DKMTDestroyDCFromMemory(const D3DKMT_DESTROYDCFROMMEMORY *desc)
{
    uint32_t dc = (uint32_t)(ULONG_PTR)desc->hDc;
    struct dc_record *r = find_record(dc);

    if (r)
    {
        if (r->guest)
        {
            /* host wined3d is shared between processes: a mapping in another one stays there (rare: a d3d11 test
             * child) */
            if (r->space == wd3d_call_memory())
                wd3d_unmap_native(r->guest, r->size);
            hostwd3d_dc_destroyed(dc);
        }
        *r = records[--record_count];
    }
    return hostwd3d_dc_delete(dc) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

/* id 3: out = {guest memory, D3DDDIFORMAT, width, height, pitch} of host DC dc; FALSE if it isn't one or the memory
 * can't be mapped. */
BOOL hostwd3d_dc_info(uint32_t dc, uint32_t out[5])
{
    struct dc_record *r = find_record(dc);

    if (!r)
        return FALSE;
    if (r->guest && r->space != wd3d_call_memory())
        return FALSE;
    if (!r->guest)
    {
        if (!(r->guest = wd3d_map_native(r->memory, r->size)))
            return FALSE;
        r->space = wd3d_call_memory();
    }
    out[0] = r->guest;
    out[1] = r->format;
    out[2] = r->width;
    out[3] = r->height;
    out[4] = r->pitch;
    return TRUE;
}
