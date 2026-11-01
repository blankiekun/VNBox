/* Host wined3d thunk: the calls gen-thunk.py leaves to hand-written code (thunk_rules.py HAND, FIXIN_STRUCTS):
 * data whose size depends on a format or a resource, maps (host memory mapped into the guest), output strings.
 * Uses wined3d's private header for formats and level sizes. Single-threaded (CSMT off): no locking. */
#include "wined3d_private.h"
#include "thunk_common.h"

/* ---- sizes ---- */

/* Bytes a w x h x d region takes with these pitches (wined3d's get_map_pitch formula, cs.c). */
static uint32_t region_size(const struct wined3d_format *format, unsigned int w, unsigned int h, unsigned int d,
        unsigned int row_pitch, unsigned int slice_pitch)
{
    if (!w || !h || !d)
        return 0;
    if (format->attrs & WINED3D_FORMAT_ATTR_PLANAR)
        return wined3d_format_calculate_size(format, 1, w, h, 1);
    return (d - 1) * slice_pitch + ((h - 1) / format->block_height) * row_pitch
            + ((w + format->block_width - 1) / format->block_width) * format->block_byte_count;
}

/* Pitches of a tightly packed region: what wined3d assumes when the guest passes 0. */
static void packed_pitch(const struct wined3d_format *format, unsigned int w, unsigned int h,
        unsigned int *row_pitch, unsigned int *slice_pitch)
{
    unsigned int r, s;

    wined3d_format_calculate_pitch(format, 1, w, h, &r, &s);
    if (!*row_pitch)
        *row_pitch = r;
    if (!*slice_pitch)
        *slice_pitch = s;
}

/* The region a box (or, for NULL, the whole sub-resource) covers. */
static void sub_resource_region(struct wined3d_resource *resource, unsigned int sub_resource_idx,
        const struct wined3d_box *box, unsigned int *w, unsigned int *h, unsigned int *d)
{
    if (box)
    {
        *w = box->right > box->left ? box->right - box->left : 0;
        *h = box->bottom > box->top ? box->bottom - box->top : 0;
        *d = box->back > box->front ? box->back - box->front : 0;
    }
    else if (resource->type == WINED3D_RTYPE_BUFFER)
    {
        *w = resource->size;
        *h = *d = 1;
    }
    else
    {
        struct wined3d_texture *texture = texture_from_resource(resource);
        unsigned int level = sub_resource_idx % texture->level_count;

        *w = wined3d_texture_get_level_width(texture, level);
        *h = wined3d_texture_get_level_height(texture, level);
        *d = wined3d_texture_get_level_depth(texture, level);
    }
    if (resource->type == WINED3D_RTYPE_BUFFER)
        *h = *d = 1;
}

/* A host copy of guest initial data (row/slice pitches kept, so the host struct only swaps the pointer). */
static void copy_sub_resource_data(struct wined3d_sub_resource_data *data, const struct wined3d_format *format,
        unsigned int w, unsigned int h, unsigned int d)
{
    uint32_t guest = (uint32_t)(uintptr_t)data->data, size;
    void *host;

    if (!guest)
        return;
    packed_pitch(format, w, h, &data->row_pitch, &data->slice_pitch);
    size = region_size(format, w, h, d, data->row_pitch, data->slice_pitch);
    host = wd3d_tmp(size);
    wd3d_read(guest, host, size);
    data->data = host;
}

/* ---- structs with guest pointers (input copies) ---- */

void wd3d_fixin_wined3d_shader_desc(struct wined3d_shader_desc *desc)
{
    uint32_t guest = (uint32_t)(uintptr_t)desc->byte_code;
    void *host;

    if (!guest)
        return;
    host = wd3d_tmp(desc->byte_code_size);
    wd3d_read(guest, host, desc->byte_code_size);
    desc->byte_code = host;
}

void wd3d_fixin_wined3d_stream_output_desc(struct wined3d_stream_output_desc *desc)
{
    struct wined3d_stream_output_element *elements;
    unsigned int i;

    if (!desc->elements)
        return;
    elements = wd3d_read_array((uint32_t)(uintptr_t)desc->elements, desc->element_count, sizeof(*elements));
    for (i = 0; i < desc->element_count; ++i)
        elements[i].semantic_name = wd3d_read_string((uint32_t)(uintptr_t)elements[i].semantic_name);
    desc->elements = elements;
}

/* ---- creates with initial data ---- */

/* (device, desc, data, parent, parent_ops, buffer **) */
uint32_t wd3d_hand_wined3d_buffer_create(void)
{
    uint32_t a_device = wd3d_arg(1), a_desc = wd3d_arg(2), a_data = wd3d_arg(3);
    uint32_t a_parent = wd3d_arg(4), a_ops = wd3d_arg(5), a_out = wd3d_arg(6);
    struct wined3d_buffer_desc desc;
    struct wined3d_sub_resource_data *data = NULL;
    struct wined3d_buffer *buffer = NULL;
    void *parent;
    HRESULT hr;

    if (!a_desc)
        return E_INVALIDARG;
    wd3d_read(a_desc, &desc, sizeof(desc));
    if (a_data)
    {
        uint32_t guest;

        data = wd3d_read_array(a_data, 1, sizeof(*data));
        if ((guest = (uint32_t)(uintptr_t)data->data))
        {
            void *host = wd3d_tmp(desc.byte_width);
            wd3d_read(guest, host, desc.byte_width);
            data->data = host;
        }
    }
    parent = hostwd3d_parent_wrap(a_parent, a_ops);
    hr = wined3d_buffer_create((struct wined3d_device *)(uintptr_t)a_device, &desc, data, parent,
            hostwd3d_parent_ops(), a_out ? &buffer : NULL);
    if (FAILED(hr))
        hostwd3d_parent_free(parent);
    if (a_out)
        wd3d_write32(a_out, (uint32_t)(uintptr_t)buffer);
    return hr;
}

/* (device, desc, layer_count, level_count, flags, data[layer_count * level_count], parent, parent_ops, texture **) */
uint32_t wd3d_hand_wined3d_texture_create(void)
{
    uint32_t a_device = wd3d_arg(1), a_desc = wd3d_arg(2), layer_count = wd3d_arg(3), level_count = wd3d_arg(4);
    uint32_t flags = wd3d_arg(5), a_data = wd3d_arg(6), a_parent = wd3d_arg(7), a_ops = wd3d_arg(8);
    uint32_t a_out = wd3d_arg(9);
    struct wined3d_device *device = (struct wined3d_device *)(uintptr_t)a_device;
    struct wined3d_resource_desc desc;
    struct wined3d_sub_resource_data *data = NULL;
    struct wined3d_texture *texture = NULL;
    void *parent;
    HRESULT hr;

    if (!a_desc || !device)
        return E_INVALIDARG;
    wd3d_read(a_desc, &desc, sizeof(desc));
    if (a_data && layer_count && level_count)
    {
        const struct wined3d_format *format = wined3d_get_format(device->adapter, desc.format, desc.bind_flags);
        unsigned int layer, level;

        data = wd3d_read_array(a_data, layer_count * level_count, sizeof(*data));
        for (layer = 0; layer < layer_count; ++layer)
        {
            for (level = 0; level < level_count; ++level)
            {
                unsigned int w = max(1, desc.width >> level), h = max(1, desc.height >> level);
                unsigned int d = desc.resource_type == WINED3D_RTYPE_TEXTURE_3D ? max(1, desc.depth >> level) : 1;

                copy_sub_resource_data(&data[layer * level_count + level], format, w, h, d);
            }
        }
    }
    parent = hostwd3d_parent_wrap(a_parent, a_ops);
    hr = wined3d_texture_create(device, &desc, layer_count, level_count, flags, data, parent, hostwd3d_parent_ops(),
            a_out ? &texture : NULL);
    if (FAILED(hr))
        hostwd3d_parent_free(parent);
    if (a_out)
        wd3d_write32(a_out, (uint32_t)(uintptr_t)texture);
    return hr;
}

/* (context, resource, sub_resource_idx, box, data, row_pitch, depth_pitch, flags) */
uint32_t wd3d_hand_wined3d_device_context_update_sub_resource(void)
{
    uint32_t a_context = wd3d_arg(1), a_resource = wd3d_arg(2), sub_resource_idx = wd3d_arg(3), a_box = wd3d_arg(4);
    uint32_t a_data = wd3d_arg(5), row_pitch = wd3d_arg(6), depth_pitch = wd3d_arg(7), flags = wd3d_arg(8);
    struct wined3d_resource *resource = (struct wined3d_resource *)(uintptr_t)a_resource;
    struct wined3d_box box, *pbox = NULL;
    unsigned int w, h, d;
    uint32_t size;
    void *host;

    if (!resource || !a_data)
        return 0;
    if (a_box)
    {
        wd3d_read(a_box, &box, sizeof(box));
        pbox = &box;
    }
    sub_resource_region(resource, sub_resource_idx, pbox, &w, &h, &d);
    if (resource->type == WINED3D_RTYPE_BUFFER)
    {
        size = w;
    }
    else
    {
        unsigned int r = row_pitch, s = depth_pitch;

        packed_pitch(resource->format, w, h, &r, &s);
        size = region_size(resource->format, w, h, d, r, s);
    }
    host = wd3d_tmp(size);
    wd3d_read(a_data, host, size);
    wined3d_device_context_update_sub_resource((struct wined3d_device_context *)(uintptr_t)a_context, resource,
            sub_resource_idx, pbox, host, row_pitch, depth_pitch, flags);
    return 0;
}

/* ---- maps: the host's mapped memory is mapped into the guest until the unmap ---- */

struct guest_map
{
    struct wined3d_resource *resource;
    unsigned int sub_resource_idx;
    uint32_t guest;
    uint32_t size;
};

static struct guest_map *maps;
static unsigned int map_count, map_size;

static struct guest_map *find_map(struct wined3d_resource *resource, unsigned int sub_resource_idx)
{
    unsigned int i;

    for (i = 0; i < map_count; ++i)
        if (maps[i].resource == resource && maps[i].sub_resource_idx == sub_resource_idx)
            return &maps[i];
    return NULL;
}

static void drop_map(struct wined3d_resource *resource, unsigned int sub_resource_idx)
{
    struct guest_map *m = find_map(resource, sub_resource_idx);

    if (!m)
        return;
    wd3d_unmap_native(m->guest, m->size);
    *m = maps[--map_count];
}

static BOOL grow_maps(void)
{
    unsigned int n = map_size ? map_size * 2 : 16;
    struct guest_map *grown = realloc(maps, n * sizeof(*maps));

    if (!grown)
        return FALSE;
    maps = grown;
    map_size = n;
    return TRUE;
}

uint32_t wd3d_mapped_guest_address(struct wined3d_resource *resource, unsigned int sub_resource_idx)
{
    struct guest_map *m = find_map(resource, sub_resource_idx);

    return m ? m->guest : 0;
}

static uint32_t map(struct wined3d_device_context *context, struct wined3d_resource *resource,
        unsigned int sub_resource_idx, uint32_t a_map_desc, uint32_t a_box, uint32_t flags)
{
    struct wined3d_map_desc map_desc = {0};
    struct wined3d_box box, *pbox = NULL;
    unsigned int w, h, d;
    uint32_t guest = 0, size;
    HRESULT hr;

    if (!resource || !a_map_desc)
        return E_INVALIDARG;
    if (a_box)
    {
        wd3d_read(a_box, &box, sizeof(box));
        pbox = &box;
    }
    if (!context)
        context = &resource->device->cs->c;
    hr = wined3d_device_context_map(context, resource, sub_resource_idx, &map_desc, pbox, flags);
    if (SUCCEEDED(hr) && map_desc.data)
    {
        sub_resource_region(resource, sub_resource_idx, pbox, &w, &h, &d);
        size = resource->type == WINED3D_RTYPE_BUFFER ? w
                : region_size(resource->format, w, h, d, map_desc.row_pitch, map_desc.slice_pitch);
        drop_map(resource, sub_resource_idx); /* a map wined3d allowed again: the old pointer is gone */
        if (!(guest = wd3d_map_native(map_desc.data, size ? size : 1)))
        {
            wined3d_device_context_unmap(context, resource, sub_resource_idx);
            hr = E_OUTOFMEMORY;
        }
        else if (map_count == map_size && !grow_maps())
        {
            wd3d_unmap_native(guest, size ? size : 1);
            wined3d_device_context_unmap(context, resource, sub_resource_idx);
            guest = 0;
            hr = E_OUTOFMEMORY;
        }
        else
        {
            maps[map_count].resource = resource;
            maps[map_count].sub_resource_idx = sub_resource_idx;
            maps[map_count].guest = guest;
            maps[map_count].size = size ? size : 1;
            ++map_count;
        }
    }
    if (FAILED(hr))
        memset(&map_desc, 0, sizeof(map_desc));
    wd3d_write32(a_map_desc, map_desc.row_pitch);
    wd3d_write32(a_map_desc + 4, map_desc.slice_pitch);
    wd3d_write32(a_map_desc + 8, guest);
    return hr;
}

static uint32_t unmap(struct wined3d_device_context *context, struct wined3d_resource *resource,
        unsigned int sub_resource_idx)
{
    if (!resource)
        return E_INVALIDARG;
    if (!context)
        context = &resource->device->cs->c;
    drop_map(resource, sub_resource_idx);
    return wined3d_device_context_unmap(context, resource, sub_resource_idx);
}

/* (resource, sub_resource_idx, map_desc, box, flags) */
uint32_t wd3d_hand_wined3d_resource_map(void)
{
    return map(NULL, (struct wined3d_resource *)(uintptr_t)wd3d_arg(1), wd3d_arg(2), wd3d_arg(3), wd3d_arg(4), wd3d_arg(5));
}

/* (resource, sub_resource_idx) */
uint32_t wd3d_hand_wined3d_resource_unmap(void)
{
    return unmap(NULL, (struct wined3d_resource *)(uintptr_t)wd3d_arg(1), wd3d_arg(2));
}

/* (context, resource, sub_resource_idx, map_desc, box, flags) */
uint32_t wd3d_hand_wined3d_device_context_map(void)
{
    return map((struct wined3d_device_context *)(uintptr_t)wd3d_arg(1), (struct wined3d_resource *)(uintptr_t)wd3d_arg(2),
            wd3d_arg(3), wd3d_arg(4), wd3d_arg(5), wd3d_arg(6));
}

/* (context, resource, sub_resource_idx) */
uint32_t wd3d_hand_wined3d_device_context_unmap(void)
{
    return unmap((struct wined3d_device_context *)(uintptr_t)wd3d_arg(1), (struct wined3d_resource *)(uintptr_t)wd3d_arg(2),
            wd3d_arg(3));
}

/* ---- outputs with guest buffers ---- */

#define MAX_STRING 4096

/* (adapter, flags, identifier): driver and description are guest buffers of driver_size / description_size bytes. */
uint32_t wd3d_hand_wined3d_adapter_get_identifier(void)
{
    uint32_t a_adapter = wd3d_arg(1), flags = wd3d_arg(2), a_id = wd3d_arg(3);
    struct wined3d_adapter_identifier id;
    uint32_t guest_driver, guest_description;
    HRESULT hr;

    if (!a_id)
        return E_INVALIDARG;
    wd3d_read(a_id, &id, sizeof(id));
    guest_driver = (uint32_t)(uintptr_t)id.driver;
    guest_description = (uint32_t)(uintptr_t)id.description;
    if (id.driver_size > MAX_STRING)
        id.driver_size = MAX_STRING;
    if (id.description_size > MAX_STRING)
        id.description_size = MAX_STRING;
    id.driver = guest_driver ? wd3d_tmp(id.driver_size) : NULL;
    id.description = guest_description ? wd3d_tmp(id.description_size) : NULL;
    if (!guest_driver)
        id.driver_size = 0;
    if (!guest_description)
        id.description_size = 0;
    hr = wined3d_adapter_get_identifier((const struct wined3d_adapter *)(uintptr_t)a_adapter, flags, &id);
    if (guest_driver)
        wd3d_write(guest_driver, id.driver, id.driver_size);
    if (guest_description)
        wd3d_write(guest_description, id.description, id.description_size);
    id.driver = (char *)(uintptr_t)guest_driver;
    id.description = (char *)(uintptr_t)guest_description;
    /* sizes as the guest gave them */
    wd3d_write(a_id + offsetof(struct wined3d_adapter_identifier, driver_version), &id.driver_version,
            sizeof(id) - offsetof(struct wined3d_adapter_identifier, driver_version));
    return hr;
}

/* (query, data, data_size, flags) */
uint32_t wd3d_hand_wined3d_query_get_data(void)
{
    uint32_t a_query = wd3d_arg(1), a_data = wd3d_arg(2), data_size = wd3d_arg(3), flags = wd3d_arg(4);
    void *data = NULL;
    HRESULT hr;

    if (data_size > 65536)
        data_size = 65536;
    if (a_data)
        data = wd3d_tmp(data_size);
    hr = wined3d_query_get_data((struct wined3d_query *)(uintptr_t)a_query, data, data_size, flags);
    if (hr == S_OK && a_data)
        wd3d_write(a_data, data, data_size);
    return hr;
}
