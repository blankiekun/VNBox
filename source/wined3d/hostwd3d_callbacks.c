/* Host wined3d: the parent ops wined3d calls, recorded for the guest stub (hostwd3d_callbacks.h). Compiled against
 * Wine's headers for the wined3d types. Single-threaded (CSMT off, jitJspi): no locking. */
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "objbase.h"
#include "wine/wined3d.h"
#include "hostwd3d_callbacks.h"

struct host_parent
{
    uint32_t guest_parent;
    uint32_t guest_ops;
};

/* device and swapchain-state parents start with this header after their wined3d base */
struct owned
{
    struct owned *next;
    uint32_t owner;
};

struct host_device_parent
{
    struct wined3d_device_parent base;
    struct owned owned;
    uint32_t guest;
};

struct host_state_parent
{
    struct wined3d_swapchain_state_parent base;
    struct owned owned;
    uint32_t guest;
};

static struct owned *owned_list; /* wrappers with an owner */

C_ASSERT(offsetof(struct host_device_parent, owned) == sizeof(void *));
C_ASSERT(offsetof(struct host_state_parent, owned) == sizeof(void *));

static void owned_unlink(struct owned *o)
{
    struct owned **p;

    for (p = &owned_list; *p; p = &(*p)->next)
        if (*p == o)
        {
            *p = o->next;
            return;
        }
}

static struct owned *owned_of(void *host_wrapper)
{
    /* both wrapper types: one pointer of wined3d base (ops), then the header */
    return (struct owned *)((char *)host_wrapper + sizeof(void *));
}

void hostwd3d_owned_parent_set(void *host_wrapper, uint32_t owner)
{
    struct owned *o, *i;

    if (!host_wrapper)
        return;
    o = owned_of(host_wrapper);
    for (i = owned_list; i; i = i->next)
        if (i == o)
        {
            o->owner = owner;
            return;
        }
    o->owner = owner;
    o->next = owned_list;
    owned_list = o;
}

void hostwd3d_owned_parent_release(uint32_t owner)
{
    struct owned **p = &owned_list;

    while (*p)
    {
        struct owned *o = *p;
        if (o->owner == owner)
        {
            *p = o->next;
            free((char *)o - sizeof(void *));
        }
        else
            p = &o->next;
    }
}

static struct hostwd3d_callback *queue;
static uint32_t queue_head, queue_count, queue_size;
static uint32_t parents_alive;

static void push(uint32_t kind, uint32_t target, uint32_t arg, uint32_t value)
{
    struct hostwd3d_callback *cb;

    if (queue_count == queue_size)
    {
        /* grow, keeping FIFO order: unwrap the ring into the new array */
        uint32_t new_size = queue_size ? queue_size * 2 : 64, i;
        struct hostwd3d_callback *n = malloc(new_size * sizeof(*n));
        if (!n)
            return;
        for (i = 0; i < queue_count; ++i)
            n[i] = queue[(queue_head + i) % queue_size];
        free(queue);
        queue = n;
        queue_head = 0;
        queue_size = new_size;
    }
    cb = &queue[(queue_head + queue_count) % queue_size];
    cb->kind = kind;
    cb->target = target;
    cb->arg = arg;
    cb->value = value;
    ++queue_count;
}

void hostwd3d_dc_destroyed(uint32_t host_dc)
{
    push(HOSTWD3D_CB_DC_DESTROYED, 0, 0, host_dc);
}

uint32_t hostwd3d_callbacks_drain(struct hostwd3d_callback *out, uint32_t max)
{
    uint32_t n = 0;

    while (n < max && queue_count)
    {
        out[n++] = queue[queue_head];
        queue_head = (queue_head + 1) % queue_size;
        --queue_count;
    }
    return n;
}

uint32_t hostwd3d_callbacks_pending(void)
{
    return queue_count;
}

uint32_t hostwd3d_parents_alive(void)
{
    return parents_alive;
}

static void __stdcall host_object_destroyed(void *parent)
{
    struct host_parent *p = parent;

    if (!p)
        return;
    if (p->guest_ops)
        push(HOSTWD3D_CB_OBJECT_DESTROYED, p->guest_ops, p->guest_parent, 0);
    free(p);
    --parents_alive;
}

static const struct wined3d_parent_ops host_parent_ops = { host_object_destroyed };

void *hostwd3d_parent_wrap(uint32_t guest_parent, uint32_t guest_parent_ops)
{
    struct host_parent *p = malloc(sizeof(*p));

    if (!p)
        return NULL;
    p->guest_parent = guest_parent;
    p->guest_ops = guest_parent_ops;
    ++parents_alive;
    return p;
}

const void *hostwd3d_parent_ops(void)
{
    return &host_parent_ops;
}

void hostwd3d_parent_free(void *host_parent)
{
    if (!host_parent)
        return;
    free(host_parent);
    --parents_alive;
}

uint32_t hostwd3d_parent_guest(const void *host_parent)
{
    return host_parent ? ((const struct host_parent *)host_parent)->guest_parent : 0;
}

static uint32_t guest_device_parent(struct wined3d_device_parent *device_parent)
{
    return CONTAINING_RECORD(device_parent, struct host_device_parent, base)->guest;
}

static void __cdecl host_device_created(struct wined3d_device_parent *device_parent, struct wined3d_device *device)
{
    hostwd3d_owned_parent_set(device_parent, (uint32_t)(uintptr_t)device);
    push(HOSTWD3D_CB_DEVICE_CREATED, guest_device_parent(device_parent), 0, (uint32_t)(uintptr_t)device);
}

static void __cdecl host_mode_changed(struct wined3d_device_parent *device_parent)
{
    push(HOSTWD3D_CB_MODE_CHANGED, guest_device_parent(device_parent), 0, 0);
}

static void __cdecl host_activate(struct wined3d_device_parent *device_parent, BOOL activate)
{
    push(HOSTWD3D_CB_ACTIVATE, guest_device_parent(device_parent), 0, activate ? 1 : 0);
}

/* The only parent call with a result: answered here as d3d11 answers it (no parent, null ops). */
static HRESULT __cdecl host_texture_sub_resource_created(struct wined3d_device_parent *device_parent,
        enum wined3d_resource_type type, struct wined3d_texture *texture, unsigned int sub_resource_idx,
        void **parent, const struct wined3d_parent_ops **parent_ops)
{
    *parent = NULL;
    *parent_ops = &host_parent_ops; /* host_object_destroyed(NULL) does nothing */
    push(HOSTWD3D_CB_SUB_RESOURCE_CREATED, guest_device_parent(device_parent), sub_resource_idx,
            (uint32_t)(uintptr_t)texture);
    return S_OK;
}

static const struct wined3d_device_parent_ops host_device_parent_ops =
{
    host_device_created,
    host_mode_changed,
    host_activate,
    host_texture_sub_resource_created,
};

void *hostwd3d_device_parent_wrap(uint32_t guest)
{
    struct host_device_parent *p = calloc(1, sizeof(*p));

    if (!p)
        return NULL;
    p->base.ops = &host_device_parent_ops;
    p->guest = guest;
    return &p->base;
}

void hostwd3d_device_parent_free(void *host_device_parent)
{
    if (!host_device_parent)
        return;
    owned_unlink(owned_of(host_device_parent));
    free(CONTAINING_RECORD(host_device_parent, struct host_device_parent, base));
}

static void __cdecl host_windowed_state_changed(struct wined3d_swapchain_state_parent *state_parent, BOOL windowed)
{
    push(HOSTWD3D_CB_WINDOWED_STATE_CHANGED, CONTAINING_RECORD(state_parent, struct host_state_parent, base)->guest,
            0, windowed ? 1 : 0);
}

static const struct wined3d_swapchain_state_parent_ops host_state_parent_ops = { host_windowed_state_changed };

void *hostwd3d_swapchain_state_parent_wrap(uint32_t guest)
{
    struct host_state_parent *p = calloc(1, sizeof(*p));

    if (!p)
        return NULL;
    p->base.ops = &host_state_parent_ops;
    p->guest = guest;
    return &p->base;
}

void hostwd3d_swapchain_state_parent_free(void *host_state_parent)
{
    if (!host_state_parent)
        return;
    owned_unlink(owned_of(host_state_parent));
    free(CONTAINING_RECORD(host_state_parent, struct host_state_parent, base));
}
