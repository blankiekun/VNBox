"""What gen-thunk.py can't read from wined3d.h: array lengths, structs that carry guest pointers, and the functions
that are hand-written (source/wined3d/thunk_hand.c) or not forwarded yet.

Parameter rules, keyed by (function, parameter):
  ('count', expr)  the pointer is an array of `expr` elements; expr names another parameter (a scalar), '*name'
                   (the guest value behind a pointer parameter, read before the call) or a number.
"""

# The functions d3d11.dll.so and dxgi.dll import (Muramasa's path). Every other export is in the stub too, but the
# host answers it with the "not forwarded" default (E_NOTIMPL / 0) and logs it once, until phase 3.
PHASE2_IMPORTS_FILE = 'phase2-imports.txt'

ARRAYS = {
    ('wined3d_device_context_set_constant_buffers', 'buffers'): 'count',
    ('wined3d_device_context_set_render_targets_and_unordered_access_views', 'render_target_views'): 'rtv_count',
    ('wined3d_device_context_set_render_targets_and_unordered_access_views', 'unordered_access_views'): 'uav_count',
    ('wined3d_device_context_set_render_targets_and_unordered_access_views', 'initial_counts'): 'uav_count',
    ('wined3d_device_context_set_samplers', 'samplers'): 'count',
    ('wined3d_device_context_set_scissor_rects', 'rects'): 'rect_count',
    ('wined3d_device_context_set_shader_resource_views', 'views'): 'count',
    ('wined3d_device_context_set_stream_outputs', 'outputs'): 'WINED3D_MAX_STREAM_OUTPUT_BUFFERS',
    ('wined3d_device_context_set_stream_sources', 'streams'): 'count',
    ('wined3d_device_context_set_unordered_access_views', 'uavs'): 'count',
    ('wined3d_device_context_set_unordered_access_views', 'initial_counts'): 'count',
    ('wined3d_device_context_set_viewports', 'viewports'): 'viewport_count',
    ('wined3d_device_context_get_scissor_rects', 'rects'): '*rect_count',
    ('wined3d_device_context_get_viewports', 'viewports'): '*viewport_count',
    ('wined3d_device_create', 'feature_levels'): 'feature_level_count',
    ('wined3d_state_create', 'levels'): 'level_count',
    ('wined3d_vertex_declaration_create', 'elements'): 'element_count',
    ('wined3d_output_set_gamma_ramp', 'ramp'): '1',
}

# Structs with guest pointers inside: after the raw copy, thunk_hand.c's wd3d_fixin_<struct>() swaps each pointer for
# a host copy (input structs only; output structs with pointers are hand-written functions).
FIXIN_STRUCTS = {'wined3d_shader_desc', 'wined3d_stream_output_desc'}

# Calls back into the guest (source/wined3d/hostwd3d_callbacks.h, P2-GL's queue): the guest object is wrapped in a host
# one whose ops only record the call for the guest stub to drain. A `void *parent` + `parent_ops` pair becomes
# hostwd3d_parent_wrap(parent, parent_ops) + hostwd3d_parent_ops(); these structs get their wrap function, and the
# wrapper is freed again when the create call fails.
CALLBACK_STRUCTS = {
    'wined3d_device_parent': ('hostwd3d_device_parent_wrap', 'hostwd3d_device_parent_free'),
    'wined3d_swapchain_state_parent': ('hostwd3d_swapchain_state_parent_wrap', 'hostwd3d_swapchain_state_parent_free'),
}
PARENT_WRAP = 'hostwd3d_parent_wrap'
PARENT_FREE = 'hostwd3d_parent_free'
PARENT_OPS = 'hostwd3d_parent_ops'
# What wined3d_*_get_parent() returns is the wrapper: mapped back to the guest's parent.
PARENT_GUEST = 'hostwd3d_parent_guest'

# Hand-written in source/wined3d/thunk_hand.c as wd3d_hand_<name>(void) returning the guest EAX: data whose size
# depends on the format or the resource, maps (host memory mapped into the guest), output strings.
HAND = {
    'wined3d_buffer_create',
    'wined3d_texture_create',
    'wined3d_device_context_update_sub_resource',
    'wined3d_resource_map',
    'wined3d_resource_unmap',
    'wined3d_device_context_map',
    'wined3d_device_context_unmap',
    'wined3d_adapter_get_identifier',
    'wined3d_query_get_data',
}

# In the phase-2 import set but not forwarded yet (video decoding, GDI interop, budget events): logged once and
# answered with the default.
# The decoder's create and profile queries are forwarded: the GL adapter has the null decoder (no profiles,
# wined3d_decoder_create answers E_INVALIDARG as the guest wined3d does), so no decoder object reaches the rest.
NOT_YET = {
    'wined3d_decoder_begin_frame', 'wined3d_decoder_decode', 'wined3d_decoder_decref',
    'wined3d_decoder_end_frame', 'wined3d_decoder_extension', 'wined3d_decoder_get_buffer',
    'wined3d_decoder_output_view_create', 'wined3d_decoder_output_view_decref',
    'wined3d_texture_get_dc', 'wined3d_texture_release_dc',
    'wined3d_adapter_register_budget_change_notification',
    'wined3d_adapter_unregister_budget_change_notification',
}

# __stdcall no-ops the guest stub does itself (the host is single threaded): never reach the host.
GUEST_ONLY = {'wined3d_mutex_lock', 'wined3d_mutex_unlock'}
# wined3d.dll's vkd3d-shader exports that d3d11 (CreateInputLayout) and d3d10 (effects) import: pure functions on guest
# memory, written in C in the stub (wined3d_guest.c), so the results live in guest memory. Not wined3d.h calls: no id,
# not in the hash.
GUEST_C = ['vkd3d_shader_free_dxbc', 'vkd3d_shader_free_shader_code', 'vkd3d_shader_free_shader_signature',
           'vkd3d_shader_parse_dxbc', 'vkd3d_shader_parse_input_signature', 'vkd3d_shader_serialize_dxbc']

# Extra host statements after the call, keyed by function: `r` is the result (absent for void functions), aN the
# guest args, waN a wrapped parent, haN a handle written back. Frees the device / swapchain-state parent wrappers
# with the object that holds them (hostwd3d_callbacks.h), and the parent wrapper set_parent replaced.
POST = {
    'wined3d_device_decref': 'if (!r) hostwd3d_owned_parent_release(a1);',
    'wined3d_swapchain_create': 'if (r == 0 && wa3) hostwd3d_owned_parent_set(wa3, (uint32_t)(uintptr_t)ha6);',
    'wined3d_swapchain_state_create': 'if (r == 0 && wa4) hostwd3d_owned_parent_set(wa4, (uint32_t)(uintptr_t)ha5);',
    'wined3d_swapchain_decref': 'if (!r) hostwd3d_owned_parent_release(a1);',
    'wined3d_swapchain_state_destroy': 'hostwd3d_owned_parent_release(a1);',
    'wined3d_resource_set_parent': 'hostwd3d_parent_free(old_parent);',
}
PRE = {
    'wined3d_resource_set_parent': 'void *old_parent = wined3d_resource_get_parent((const struct wined3d_resource*)(uintptr_t)a1);',
}

# Guest stub: what an export does before trapping to the host. ('window', n): argument n is an HWND the host must
# know (registered with id 1: X drawable + client rect); ('desc', n): argument n is a wined3d_swapchain_desc whose
# device_window is; ('refresh',): re-register every window registered so far (sizes may have changed).
GUEST_PRE = {
    'wined3d_device_create': [('window', 4)],
    'wined3d_device_acquire_focus_window': [('window', 2)],
    'wined3d_device_reset': [('desc', 2)],
    'wined3d_register_window': [('window', 2)],
    'wined3d_swapchain_create': [('desc', 2)],
    'wined3d_swapchain_state_create': [('desc', 1), ('window', 2)],
    'wined3d_swapchain_state_set_fullscreen': [('desc', 2)],
    'wined3d_swapchain_set_window': [('window', 2)],
    'wined3d_swapchain_present': [('window', 4)],
    'wined3d_swapchain_resize_buffers': [('refresh',)],
    'wined3d_swapchain_state_resize_target': [('refresh',)],
}
# Guest stub: what an export does after the host call (and its callbacks), keyed by function: (fn, n) calls the
# guest's fn(argument n, result) and returns the result. The output desc's HMONITOR is the host user32's handle:
# dxgi matches it against the guest's MonitorFromWindow (dxgi_get_output_from_window), so the guest swaps in its own.
GUEST_POST = {
    'wined3d_output_get_desc': ('_wd3d_output_desc_post', 2),
}
# Bumped when the guest/host contract changes beyond the signatures (hello's arguments, the call-id set, callbacks).
PROTOCOL_REVISION = 'v3: hello(hash, &pending); drain one at a time; window registration with the window rect'
