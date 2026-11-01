/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "boxedwine.h"

#if defined(__TEST) && defined(BOXEDWINE_HOST_WINED3D)

// Host wined3d (HOST_WINED3D=1, source/wined3d): every wined3d_* export is in the link, DllMain attaches with the
// shims (settings from the registry table, TLS), and a wined3d object without 3D (WINED3D_NO3D: no GL, one
// synthetic WebGL output) is created and released. Needs no window or GL, so it runs in node.
#include "testHostWined3d.h"
#include "testCPU.h"

extern "C" {
struct wd3d_export {
    const char* name;
    void* fn;
};
const wd3d_export* wd3d_link_root(unsigned int* count);
int wd3d_dll_main(unsigned int reason);
void* wined3d_create(uint32_t flags);
unsigned int wined3d_decref(void* wined3d);
unsigned int wined3d_get_adapter_count(const void* wined3d);
void* GetModuleHandleA(const char*);
void* GetProcAddress(void*, const char*);
void* wd3d_opengl32_proc(const char* name);
}

void testHostWined3dNo3d() {
    const unsigned int DLL_PROCESS_DETACH = 0, DLL_PROCESS_ATTACH = 1, WINED3D_NO3D = 2;
    unsigned int count = 0;
    const wd3d_export* exports = wd3d_link_root(&count);

    if (count != 328) testFail("%u wined3d exports, expected 328 (wined3d.spec)", count);
    for (unsigned int i = 0; i < count; i++) {
        if (!exports[i].fn) testFail("export %s is null", exports[i].name);
    }
    if (!wd3d_dll_main(DLL_PROCESS_ATTACH)) {
        testFail("DllMain(DLL_PROCESS_ATTACH) failed");
        return;
    }
    // opengl32's GetProcAddress answers every GL name wined3d's table asks for, also names stored below 64 KB
    // (wasm static data: a HIWORD() ordinal check once dropped glGetIntegerv)
    static const char lowName[] = "glGetIntegerv";
    if (!GetProcAddress(GetModuleHandleA("opengl32.dll"), lowName) || GetProcAddress(GetModuleHandleA("opengl32.dll"), lowName) != wd3d_opengl32_proc(lowName))
        testFail("opengl32 GetProcAddress(glGetIntegerv) is not the bridge's");
    void* wined3d = wined3d_create(WINED3D_NO3D);
    if (!wined3d) {
        testFail("wined3d_create(WINED3D_NO3D) failed");
    } else {
        if (wined3d_get_adapter_count(wined3d) != 1) testFail("%u adapters", wined3d_get_adapter_count(wined3d));
        if (wined3d_decref(wined3d)) testFail("wined3d still referenced after its last release");
    }
    if (!wd3d_dll_main(DLL_PROCESS_DETACH)) testFail("DllMain(DLL_PROCESS_DETACH) failed");
}

// The guest->host thunk (thunkHost.cpp + the generated dispatcher + thunk_hand.c), driven like the guest stub drives
// it: cdecl args on the guest stack, then the int 0x99 index. Hello, a no-3D wined3d, refcounts, a struct output with
// guest string buffers, a device whose device_created callback is queued for the guest, a buffer and a texture with
// initial data mapped into the guest, and parents that come back as the guest's.
#include "../../wined3d/thunkHost.h"
#include "../../wined3d/thunk_ids.h"

#define cpu (testContext().cpu)
#define memory (testContext().memory)

namespace {

constexpr U32 HEAP = 0xF0000000; // 32 RW pages (testCPU.cpp)
constexpr U32 STACK = HEAP + 0x400, OUTP = HEAP + 0x500, IDENT = HEAP + 0x600, DRIVER = HEAP + 0x700, DESCRIPTION = HEAP + 0x800;
constexpr U32 DESC = HEAP + 0x900, DATA = HEAP + 0x1000, MAP = HEAP + 0x980, CBS = HEAP + 0xA00, LEVELS = HEAP + 0x9C0;
constexpr U32 PENDING = HEAP + 0x5F0, SWAP = HEAP + 0x2000, DATA2 = HEAP + 0x2100;
constexpr U32 RET = 0x12345678;

U32 call(U32 id, std::initializer_list<U32> args) {
    ESP = STACK;
    memory->writed(STACK, RET);
    U32 slot = STACK + 4;
    for (U32 a : args) {
        memory->writed(slot, a);
        slot += 4;
    }
    EAX = 0xDEADBEEF;
    hostWined3dCall(cpu, WD3D_CALL_BASE + id);
    if (ESP != STACK) testFail("call %u moved ESP", id);
    return EAX;
}

// Drains the callback queue into guest memory; returns how many and checks the first one.
U32 drain(U32 kind, U32 target, U32 arg) {
    U32 n = call(WD3D_ID_DRAIN_CALLBACKS, { CBS, 16 });
    bool found = false;
    for (U32 i = 0; i < n; i++) {
        U32 base = CBS + i * 16;
        if (memory->readd(base) == kind && memory->readd(base + 4) == target && memory->readd(base + 8) == arg) found = true;
    }
    if (!found) testFail("callback kind %u for %.8X not queued (%u drained)", kind, target, n);
    return n;
}

}

void testHostWined3dThunk() {
    const U32 WINED3D_NO3D = 2, HAL = 1, LEVEL_11 = 0xb000, R8G8B8A8_UNORM = 58, TEXTURE_2D = 3;
    const U32 ACCESS_GPU_CPU_RW = 0x1 | 0x2 | 0x4 | 0x8, MAP_RW = 0x40000000 | 0x80000000;
    const U32 CB_OBJECT_DESTROYED = 1, CB_DEVICE_CREATED = 2;
    testNewInstruction(0);
    for (int s : { CS, DS, ES, SS }) cpu->seg[s].address = 0;
    memory->memset(HEAP, 0, 32 * K_PAGE_SIZE);

    if (call(WD3D_ID_HELLO, { 0x1234 }) != 0) testFail("hello accepted a wrong protocol hash");
    memory->writed(PENDING, 0xFFFF);
    if (call(WD3D_ID_HELLO, { WD3D_PROTOCOL_HASH, PENDING }) != 1) testFail("hello refused the protocol hash");
    while (call(WD3D_ID_DRAIN_CALLBACKS, { CBS, 16 })) {} // whatever earlier tests left
    if (memory->readd(PENDING)) testFail("the stub's pending count is not 0 after hello + drain");

    U32 wined3d = call(WD3D_ID_wined3d_create, { WINED3D_NO3D });
    if (!wined3d) {
        testFail("wined3d_create(WINED3D_NO3D) through the thunk failed");
        return;
    }
    if (call(WD3D_ID_wined3d_get_adapter_count, { wined3d }) != 1) testFail("adapter count");
    // wined3d_incref is not in the phase-2 set: answered 0 by default, wined3d untouched
    if (call(WD3D_ID_wined3d_incref, { wined3d }) != 0) testFail("phase-3 wined3d_incref was forwarded");
    U32 adapter = call(WD3D_ID_wined3d_get_adapter, { wined3d, 0 });
    if (!adapter) testFail("no adapter");

    // struct output: guest string buffers stay the guest's, the strings land in them
    memory->writed(IDENT, DRIVER);
    memory->writed(IDENT + 4, 64);
    memory->writed(IDENT + 8, DESCRIPTION);
    memory->writed(IDENT + 12, 64);
    memory->writed(IDENT + 24, 0xAAAAAAAA); // vendor_id
    if (call(WD3D_ID_wined3d_adapter_get_identifier, { adapter, 0, IDENT })) testFail("adapter_get_identifier failed");
    if (memory->readd(IDENT) != DRIVER || memory->readd(IDENT + 8) != DESCRIPTION || memory->readd(IDENT + 4) != 64) testFail("identifier's guest pointers changed");
    if (memory->readd(IDENT + 24) == 0xAAAAAAAA) testFail("identifier fields not written");
    if (!memory->strlen(DESCRIPTION)) testFail("empty adapter description");

    // device: device_parent is the guest's, device_created reaches the guest's queue
    // no-3D has a low feature level; d3d11 asks for its list and gets the best one
    const U32 levels[5] = { LEVEL_11, 0xa000, 0x9100, 0x7000, 0x5000 };
    memory->memcpy(LEVELS, levels, sizeof(levels));
    memory->writed(OUTP, 0);
    U32 hr = call(WD3D_ID_wined3d_device_create, { wined3d, adapter, HAL, 0, 0, 4, LEVELS, 5, 0x00D0D0D0, OUTP });
    U32 device = memory->readd(OUTP);
    if (hr || !device) {
        testFail("wined3d_device_create through the thunk: hr %.8X", hr);
        call(WD3D_ID_wined3d_decref, { wined3d });
        return;
    }
    drain(CB_DEVICE_CREATED, 0x00D0D0D0, 0);
    U32 context = call(WD3D_ID_wined3d_device_get_immediate_context, { device });
    if (!context) testFail("no immediate context");

    // buffer with initial data, mapped into the guest
    for (U32 i = 0; i < 64; i++) memory->writeb(DATA + i, (U8)(i * 3));
    U32 desc[6] = { 64, 0, 0x1 /* vertex buffer */, ACCESS_GPU_CPU_RW, 0, 0 };
    memory->memcpy(DESC, desc, sizeof(desc));
    memory->writed(DESC + 0x40, DATA); // sub_resource_data { data, row_pitch, slice_pitch }
    memory->writed(DESC + 0x44, 0);
    memory->writed(DESC + 0x48, 0);
    memory->writed(OUTP, 0);
    hr = call(WD3D_ID_wined3d_buffer_create, { device, DESC, DESC + 0x40, 0x5555, 0x6666, OUTP });
    U32 buffer = memory->readd(OUTP);
    if (hr || !buffer) testFail("buffer_create: hr %.8X", hr);
    if (buffer) {
        if (call(WD3D_ID_wined3d_buffer_incref, { buffer }) != 2) testFail("buffer incref");
        if (call(WD3D_ID_wined3d_buffer_decref, { buffer }) != 1) testFail("buffer decref");
        if (call(WD3D_ID_wined3d_buffer_get_parent, { buffer }) != 0x5555) testFail("buffer parent is not the guest's");
        U32 resource = call(WD3D_ID_wined3d_buffer_get_resource, { buffer });
        if (call(WD3D_ID_wined3d_resource_map, { resource, 0, MAP, 0, MAP_RW })) testFail("buffer map failed");
        U32 mapped = memory->readd(MAP + 8);
        if (!mapped) testFail("buffer map gave no guest address");
        else {
            for (U32 i = 0; i < 64; i++) {
                if (memory->readb(mapped + i) != (U8)(i * 3)) {
                    testFail("mapped buffer byte %u is %u, not the initial data", i, memory->readb(mapped + i));
                    break;
                }
            }
            memory->writeb(mapped + 5, 0x77);
        }
        if (call(WD3D_ID_wined3d_resource_unmap, { resource, 0 })) testFail("buffer unmap failed");
        if (call(WD3D_ID_wined3d_device_context_map, { context, resource, 0, MAP, 0, 0x80000000 })) testFail("context map failed");
        mapped = memory->readd(MAP + 8);
        if (!mapped || memory->readb(mapped + 5) != 0x77) testFail("guest write through the map was lost");
        if (call(WD3D_ID_wined3d_device_context_unmap, { context, resource, 0 })) testFail("context unmap failed");
        if (call(WD3D_ID_wined3d_buffer_decref, { buffer })) testFail("buffer still referenced");
        if (memory->readd(PENDING) != 1) testFail("pending count after a destroy is %u, not 1", memory->readd(PENDING));
        drain(CB_OBJECT_DESTROYED, 0x6666, 0x5555);
        if (memory->readd(PENDING)) testFail("pending count not cleared by the drain");
    }

    // 4x4 RGBA staging texture: created, parent and destroy callback are the guest's. (Initial data, maps and
    // update_sub_resource on textures go through a context, which a no-3D device only has once a swapchain called
    // init_3d: not covered here; milestone 2 runs them on the GL adapter.)
    U32 tdesc[11] = { TEXTURE_2D, R8G8B8A8_UNORM, 0, 0, 0, 0, 0x2 | 0x4 | 0x8 /* staging: CPU, map r/w */, 4, 4, 1, 0 };
    memory->memcpy(DESC, tdesc, sizeof(tdesc));
    memory->writed(OUTP, 0);
    hr = call(WD3D_ID_wined3d_texture_create, { device, DESC, 1, 1, 0, 0, 0x7777, 0x8888, OUTP });
    U32 texture = memory->readd(OUTP);
    if (hr || !texture) testFail("texture_create: hr %.8X", hr);
    if (texture) {
        if (call(WD3D_ID_wined3d_texture_get_parent, { texture }) != 0x7777) testFail("texture parent is not the guest's");
        U32 resource = call(WD3D_ID_wined3d_texture_get_resource, { texture });
        call(WD3D_ID_wined3d_resource_get_desc, { resource, DESC + 0x80 }); // struct out
        if (memory->readd(DESC + 0x80) != TEXTURE_2D || memory->readd(DESC + 0x84) != R8G8B8A8_UNORM
                || memory->readd(DESC + 0x9C) != 4 || memory->readd(DESC + 0xA0) != 4) testFail("resource_get_desc");
        if (call(WD3D_ID_wined3d_texture_from_resource, { resource }) != texture) testFail("texture_from_resource");
        if (call(WD3D_ID_wined3d_texture_decref, { texture })) testFail("texture still referenced");
        drain(CB_OBJECT_DESTROYED, 0x8888, 0x7777);
    }

    // A swapchain on a registered window: the device's first (implicit) one runs init_3d, so the device has a context
    // and the texture paths of thunk_hand.c run: create with initial data (sized from the format), map/unmap of a
    // sub-resource, update_sub_resource with a box and a row pitch. The no-3D adapter can't upload texture data
    // (wined3d_texture_no3d_upload_data is a FIXME), so contents are only checked through the guest's own map;
    // uploads that land are exercised on the GL adapter (the game).
    const U32 HWND_TEST = 0x10024, B8G8R8A8_UNORM = 118, SWAP_EFFECT_DISCARD = 0;
    call(WD3D_ID_REGISTER_WINDOW, { HWND_TEST, 0, 0, 0, 8, 8, 1, 0, 0, 8, 8 });
    U32 output = call(WD3D_ID_wined3d_adapter_get_output, { adapter, 0 });
    U32 sdesc[16] = { output, 8, 8, B8G8R8A8_UNORM, 1, 0 /* no-3D formats have no bind caps */, 0, 0, SWAP_EFFECT_DISCARD, HWND_TEST, 1, 0, 0, 0x10000 /* IMPLICIT: init_3d */, 0, 0 };
    memory->memcpy(SWAP, sdesc, sizeof(sdesc));
    memory->writed(OUTP, 0);
    hr = call(WD3D_ID_wined3d_swapchain_create, { device, SWAP, 0x00E0E0E0, 0x9999, 0xAAAA, OUTP });
    U32 swapchain = memory->readd(OUTP);
    if (hr || !swapchain) testFail("swapchain_create: hr %.8X", hr);
    if (swapchain) {
        if (!call(WD3D_ID_wined3d_swapchain_get_back_buffer, { swapchain, 0 })) testFail("swapchain has no back buffer");
        U32 rows[16];
        for (U32 i = 0; i < 16; i++) rows[i] = 0x01010101u * i;
        memory->memcpy(DATA, rows, sizeof(rows));
        U32 t2[11] = { TEXTURE_2D, R8G8B8A8_UNORM, 0, 0, 0, 0, 0x2 | 0x4 | 0x8 /* CPU, map r/w */, 4, 4, 1, 0 };
        memory->memcpy(DESC, t2, sizeof(t2));
        memory->writed(DESC + 0x40, DATA); // sub_resource_data: 4 rows of 16 bytes
        memory->writed(DESC + 0x44, 16);
        memory->writed(DESC + 0x48, 64);
        memory->writed(OUTP, 0);
        hr = call(WD3D_ID_wined3d_texture_create, { device, DESC, 1, 1, 0, DESC + 0x40, 0x7272, 0x8282, OUTP });
        U32 tex = memory->readd(OUTP);
        if (hr || !tex) testFail("texture_create with data: hr %.8X", hr);
        if (tex) {
            if (call(WD3D_ID_wined3d_texture_decref, { tex })) testFail("data texture still referenced");
            drain(CB_OBJECT_DESTROYED, 0x8282, 0x7272);
        }
        // maps on one without data (no-3D leaves a texture whose upload it dropped without a valid location)
        memory->writed(OUTP, 0);
        hr = call(WD3D_ID_wined3d_texture_create, { device, DESC, 1, 1, 0, 0, 0x7171, 0x8181, OUTP });
        tex = memory->readd(OUTP);
        if (hr || !tex) testFail("texture_create: hr %.8X", hr);
        if (tex) {
            U32 resource = call(WD3D_ID_wined3d_texture_get_resource, { tex });
            if (call(WD3D_ID_wined3d_resource_map, { resource, 0, MAP, 0, 0x40000000 | 0x80000000 })) testFail("texture map failed");
            U32 pitch = memory->readd(MAP), mapped = memory->readd(MAP + 8);
            if (!mapped || pitch < 16) testFail("texture map: data %.8X pitch %u", mapped, pitch);
            else memory->writed(mapped + pitch * 3 + 4, 0x5A5A5A5A);
            if (call(WD3D_ID_wined3d_resource_unmap, { resource, 0 })) testFail("texture unmap failed");
            if (call(WD3D_ID_wined3d_resource_unmap, { resource, 0 }) == 0) testFail("second unmap succeeded");
            if (call(WD3D_ID_wined3d_resource_map, { resource, 0, MAP, 0, 0x80000000 })) testFail("texture map 2 failed");
            pitch = memory->readd(MAP);
            mapped = memory->readd(MAP + 8);
            if (!mapped || memory->readd(mapped + pitch * 3 + 4) != 0x5A5A5A5A) testFail("guest write through the texture map was lost");
            call(WD3D_ID_wined3d_resource_unmap, { resource, 0 });
            // rows 1..2, columns 1..2 from a buffer with a 32-byte row pitch (thunk_hand reads 32 + 8 bytes)
            for (U32 i = 0; i < 16; i++) memory->writed(DATA2 + i * 4, 0xA0000000u + i);
            U32 box[6] = { 1, 1, 3, 3, 0, 1 };
            memory->memcpy(DESC + 0xC0, box, sizeof(box));
            call(WD3D_ID_wined3d_device_context_update_sub_resource, { context, resource, 0, DESC + 0xC0, DATA2, 32, 64, 0 });
            call(WD3D_ID_wined3d_device_context_update_sub_resource, { context, resource, 0, 0, DATA, 16, 64, 0 }); // whole level
            if (call(WD3D_ID_wined3d_device_context_map, { context, resource, 0, MAP, 0, 0x80000000 })) testFail("texture context map failed");
            if (!memory->readd(MAP + 8)) testFail("texture context map gave no guest address");
            if (call(WD3D_ID_wined3d_device_context_unmap, { context, resource, 0 })) testFail("texture context unmap failed");
            if (call(WD3D_ID_wined3d_texture_decref, { tex })) testFail("data texture still referenced");
            drain(CB_OBJECT_DESTROYED, 0x8181, 0x7171);
        }
        if (call(WD3D_ID_wined3d_swapchain_decref, { swapchain })) testFail("swapchain still referenced");
        drain(CB_OBJECT_DESTROYED, 0xAAAA, 0x9999);
    }

    // a phase-3 export answers its default
    if (call(WD3D_ID_wined3d_device_begin_scene, { device }) != 0x80004001) testFail("unforwarded call did not answer E_NOTIMPL");
    if (call(WD3D_ID_wined3d_device_decref, { device })) testFail("device still referenced");
    if (call(WD3D_ID_wined3d_decref, { wined3d })) testFail("wined3d still referenced");
    while (call(WD3D_ID_DRAIN_CALLBACKS, { CBS, 16 })) {}

    // Real guest code: the stub's export as gen-thunk.py emits it (int $0x99; .long index; ret), called cdecl,
    // run by the CPU (JIT in testJit), so the decoder/JIT int 0x99 path reaches host wined3d.
    testNewInstruction(0);
    U32 index = WD3D_CALL_BASE + WD3D_ID_wined3d_create;
    const U8 code[] = {
        0xEB, 0x08,                                                     // jmp over the stub
        0xCD, 0x99, (U8)index, (U8)(index >> 8), (U8)(index >> 16), (U8)(index >> 24), 0xC3, // _wined3d_create
        0x90,
        0x6A, (U8)WINED3D_NO3D,                                         // push WINED3D_NO3D
        0xE8, 0xF1, 0xFF, 0xFF, 0xFF,                                   // call _wined3d_create (offset 2)
        0x83, 0xC4, 0x04,                                               // add esp, 4
    };
    for (U8 b : code) testPushCode8(b);
    U32 esp = ESP;
    testRunCPU();
    if (ESP != esp) testFail("guest stack unbalanced after the stub call");
    wined3d = EAX;
    if (!wined3d) testFail("guest wined3d_create through int 0x99 returned NULL");
    else {
        for (int seg : { CS, DS, ES, SS }) cpu->seg[seg].address = 0; // call() uses flat addresses
        if (call(WD3D_ID_wined3d_get_adapter_count, { wined3d }) != 1) testFail("guest-created wined3d has no adapter");
        if (call(WD3D_ID_wined3d_decref, { wined3d })) testFail("guest-created wined3d still referenced");
    }
}

#undef cpu
#undef memory

#endif
