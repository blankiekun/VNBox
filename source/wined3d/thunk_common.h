/* Host wined3d thunk (HOST_WINED3D=1): what thunk_dispatch.c (generated) and thunk_hand.c share. Compiled with Wine's
 * headers and flags like the other C shims; the guest memory accessors are implemented on the Boxedwine side
 * (thunkHost.cpp) and only pass fixed-width integers and host pointers. Guest parents go through P2-GL's wrappers
 * (hostwd3d_callbacks.h); the guest stub drains their queued callbacks with WD3D_ID_DRAIN_CALLBACKS. Valid only inside a call: the CPU they read
 * is the one that executed the stub's int 0x99, and wd3d_tmp() memory is freed when the call returns. */
#ifndef WD3D_THUNK_COMMON_H
#define WD3D_THUNK_COMMON_H

#define COBJMACROS
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "objbase.h"
#include "wine/wined3d.h"
#include "thunk_ids.h"
#include "hostwd3d_callbacks.h"

/* thunkHost.cpp */
uint32_t wd3d_arg(uint32_t n); /* n-th 32-bit stack slot of the guest call: 0 = return address, 1 = first arg */
uint32_t wd3d_read32(uint32_t guest);
void wd3d_write32(uint32_t guest, uint32_t value);
void wd3d_read(uint32_t guest, void *dst, uint32_t len);
void wd3d_write(uint32_t guest, const void *src, uint32_t len);
void *wd3d_tmp(uint32_t len);                                          /* zeroed, freed after the call */
void *wd3d_read_array(uint32_t guest, uint32_t count, uint32_t size);  /* wd3d_tmp copy; count capped at 65536 */
const char *wd3d_read_string(uint32_t guest);                          /* wd3d_tmp copy, NULL for 0 */
uint32_t wd3d_map_native(void *host, uint32_t len);                    /* host memory into the guest, 0 = failed */
void wd3d_unmap_native(uint32_t guest, uint32_t len);
uint32_t wd3d_not_forwarded(uint32_t id, uint32_t ret);                 /* logs the first call of each id */

static inline float wd3d_f32(uint32_t bits)
{
    union { uint32_t u; float f; } v;
    v.u = bits;
    return v.f;
}

uint32_t wd3d_dispatch(uint32_t id);
extern const char *const wd3d_call_names[WD3D_ID_COUNT];

/* thunk_hand.c: structs with guest pointers inside (input copies), and the hand-written calls. */
void wd3d_fixin_wined3d_shader_desc(struct wined3d_shader_desc *desc);
void wd3d_fixin_wined3d_stream_output_desc(struct wined3d_stream_output_desc *desc);
uint32_t wd3d_hand_wined3d_buffer_create(void);
uint32_t wd3d_hand_wined3d_texture_create(void);
uint32_t wd3d_hand_wined3d_device_context_update_sub_resource(void);
uint32_t wd3d_hand_wined3d_resource_map(void);
uint32_t wd3d_hand_wined3d_resource_unmap(void);
uint32_t wd3d_hand_wined3d_device_context_map(void);
uint32_t wd3d_hand_wined3d_device_context_unmap(void);
uint32_t wd3d_hand_wined3d_adapter_get_identifier(void);
uint32_t wd3d_hand_wined3d_query_get_data(void);

/* For tests: the guest mapping a host map produced (0 when none). */
uint32_t wd3d_mapped_guest_address(struct wined3d_resource *resource, unsigned int sub_resource_idx);

#endif
