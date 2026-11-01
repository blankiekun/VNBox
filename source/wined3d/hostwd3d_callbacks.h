/* Host wined3d: calls wined3d makes into its parent (d3d8/d3d9/d3d11/dxgi/ddraw, guest code), queued.
 *
 * Boxedwine can't call guest code from host code and get the result back, so the host never calls the guest's
 * parent ops. The guest stub wraps what it passes in (hostwd3d_parent_wrap & co.), and wined3d calls the host ops
 * those wrappers carry; they only record a hostwd3d_callback. After every call into host wined3d returns, the guest
 * stub drains the queue (hostwd3d_callbacks_drain) and makes the calls itself, in order, until it is empty (a drained
 * callback may call wined3d again and queue more).
 *
 * Why later is safe: native wined3d with CSMT already destroys objects on its command-stream thread, so the parents'
 * object_destroyed already runs after the call that dropped the last reference; wined3d_device_created runs at the end
 * of wined3d_device_create, which the stub drains before the guest's caller continues. The one callback that returns
 * something, texture_sub_resource_created, gets a host default (no parent, null ops): exactly what d3d11 returns.
 * d3d8/d3d9/ddraw build a surface object there (phase 3); the queued record tells the stub that it happened.
 *
 * Guest pointers are uint32_t. Host pointers (wined3d objects) fit in 32 bits too (wasm32). */
#ifndef HOSTWD3D_CALLBACKS_H
#define HOSTWD3D_CALLBACKS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum hostwd3d_callback_kind
{
    /* guest: ((struct wined3d_parent_ops *)target)->wined3d_object_destroyed(arg)  (stdcall) */
    HOSTWD3D_CB_OBJECT_DESTROYED = 1,
    /* guest: device_parent = target; device_parent->ops->wined3d_device_created(device_parent, <guest handle of the
     * host wined3d_device in value>) */
    HOSTWD3D_CB_DEVICE_CREATED,
    /* guest: device_parent->ops->mode_changed(device_parent), device_parent = target */
    HOSTWD3D_CB_MODE_CHANGED,
    /* guest: device_parent->ops->activate(device_parent, value), device_parent = target */
    HOSTWD3D_CB_ACTIVATE,
    /* guest: state_parent->ops->windowed_state_changed(state_parent, value), state_parent = target */
    HOSTWD3D_CB_WINDOWED_STATE_CHANGED,
    /* already answered on the host (no parent, null ops); target = guest device_parent, value = host texture,
     * arg = sub-resource index. Nothing to call for d3d11/dxgi. */
    HOSTWD3D_CB_SUB_RESOURCE_CREATED,
    /* guest: the DC it made for host DC value (wined3d_texture_get_dc) is gone on the host (release_dc, texture
     * destroyed): destroy the guest's own DC. */
    HOSTWD3D_CB_DC_DESTROYED,
};

struct hostwd3d_callback
{
    uint32_t kind;   /* enum hostwd3d_callback_kind */
    uint32_t target; /* guest parent_ops / device_parent / swapchain_state_parent */
    uint32_t arg;    /* guest parent (OBJECT_DESTROYED), sub-resource index (SUB_RESOURCE_CREATED) */
    uint32_t value;  /* host wined3d_device / texture, or the BOOL argument */
};

/* A parent + parent_ops pair for a wined3d create call (buffer, texture, view, shader, state, query, swapchain, ...):
 * pass the returned pointer as parent and hostwd3d_parent_ops() as parent_ops. guest_parent_ops 0 means nothing to
 * call back (the guest's null ops). wined3d_*_get_parent() then returns the wrapper: hostwd3d_parent_guest() maps it
 * back. The wrapper is freed when wined3d destroys the object. */
void *hostwd3d_parent_wrap(uint32_t guest_parent, uint32_t guest_parent_ops);
const void *hostwd3d_parent_ops(void); /* const struct wined3d_parent_ops * */
uint32_t hostwd3d_parent_guest(const void *host_parent); /* 0 for NULL */
/* A wrapper wined3d never took (its create call failed): freed without a callback. */
void hostwd3d_parent_free(void *host_parent);

/* The device_parent for wined3d_device_create (struct wined3d_device_parent *); lives until hostwd3d_device_parent_free. */
void *hostwd3d_device_parent_wrap(uint32_t guest_device_parent);
void hostwd3d_device_parent_free(void *host_device_parent);
/* The state_parent for wined3d_swapchain_state_create (struct wined3d_swapchain_state_parent *). */
void *hostwd3d_swapchain_state_parent_wrap(uint32_t guest_state_parent);
void hostwd3d_swapchain_state_parent_free(void *host_state_parent);

/* Device / swapchain-state parent wrappers live as long as the object holding them: set_ records the owner (a
 * wined3d_device: done by device_created itself; a swapchain or swapchain_state: after its create), release_ frees
 * the wrappers of an owner that is gone (final device/swapchain decref, swapchain_state_destroy). */
void hostwd3d_owned_parent_set(void *host_wrapper, uint32_t owner);
void hostwd3d_owned_parent_release(uint32_t owner);

/* Queues HOSTWD3D_CB_DC_DESTROYED (d3dkmt.c, for DCs a guest asked about with id 3). */
void hostwd3d_dc_destroyed(uint32_t host_dc);

/* Moves up to max queued callbacks, oldest first, into out; returns how many. */
uint32_t hostwd3d_callbacks_drain(struct hostwd3d_callback *out, uint32_t max);
uint32_t hostwd3d_callbacks_pending(void);
/* Wrappers alive (leak checks, tests). */
uint32_t hostwd3d_parents_alive(void);

#ifdef __cplusplus
}
#endif

#endif
