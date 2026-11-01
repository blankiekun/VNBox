/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

// Host wined3d thunk, Boxedwine side: the guest stub's int 0x99 (index WD3D_CALL_BASE + id) runs the generated
// dispatcher (thunk_dispatch.c) on the host, and the guest memory accessors it uses (thunk_common.h) read the
// calling CPU's memory. Everything a call allocates with wd3d_tmp() is freed when it returns.
#include "boxedwine.h"

#ifdef BOXEDWINE_HOST_WINED3D
#include "thunkHost.h"
#include "thunk_ids.h"
#include "hostwd3d_bridge.h"
#include "hostwd3d_callbacks.h"
#include "vnboxstats.h"
#include <stdlib.h>
#include <algorithm>
#include <vector>

static_assert(sizeof(hostwd3d_callback) == 16, "guest layout");
static_assert(HOSTWD3D_CALL_BASE == WD3D_CALL_BASE && HOSTWD3D_CALL_LIMIT == WD3D_CALL_LIMIT, "thunkHost.h vs thunk_ids.h");

extern "C" {
uint32_t wd3d_dispatch(uint32_t id);
extern const char* const wd3d_call_names[WD3D_ID_COUNT];
int wd3d_dll_main(unsigned int reason);
}

namespace {

constexpr U32 MAX_ARRAY_COUNT = 65536;

thread_local CPU* callCpu;
thread_local std::vector<void*>* callTmp;
bool attached;
bool loggedOnce[WD3D_ID_COUNT];
// The stub's _wd3d_pending (hello's second argument) in the memory of the process that said hello: set to the
// number of queued callbacks after every call, so an export only asks for them (id 2) when there are some.
// Process-wide, not per thread: whichever thread queues a callback, the next export the guest returns from sees it.
KMemory* pendingMemory;
U32 pendingGuest;
U32 pendingWritten;
// stats=1: calls and host ms per id in this [stats] window (nested calls count in both).
U32 statCalls[WD3D_ID_COUNT];
double statMs[WD3D_ID_COUNT];

// One per call, nested calls (a callback that calls wined3d again) included: frees the call's wd3d_tmp() memory
// and restores the outer call's CPU and arena, also when a guest fault unwinds through the call.
struct CallScope {
    CPU* outerCpu;
    std::vector<void*>* outerTmp;
    std::vector<void*> tmp;
    explicit CallScope(CPU* cpu) : outerCpu(callCpu), outerTmp(callTmp) {
        callCpu = cpu;
        callTmp = &tmp;
    }
    ~CallScope() {
        for (void* p : tmp) free(p);
        callCpu = outerCpu;
        callTmp = outerTmp;
    }
};

bool attach() {
    if (!attached) {
        attached = wd3d_dll_main(1 /* DLL_PROCESS_ATTACH */) != 0;
        if (!attached) klog("[wined3d] host DllMain(DLL_PROCESS_ATTACH) failed");
    }
    return attached;
}

U32 hello(U32 hash, U32 pending) {
    if (hash != WD3D_PROTOCOL_HASH) {
        klog_fmt("[wined3d] guest wined3d.dll speaks protocol %.8X, host %.8X: refusing the load", hash, WD3D_PROTOCOL_HASH);
        return 0;
    }
    if (!attach()) return 0;
    if (pending) {
        if (pendingGuest && pendingMemory != callCpu->memory) {
            klog("[wined3d] a second process loaded the wined3d stub: host wined3d is shared, callbacks follow the latest");
        }
        pendingMemory = callCpu->memory;
        pendingGuest = pending;
        pendingWritten = 0;
        pendingMemory->writed(pendingGuest, 0);
    }
    return 1;
}

void updatePending() {
    if (!pendingGuest || pendingMemory != callCpu->memory) return;
    U32 n = hostwd3d_callbacks_pending();
    if (n != pendingWritten) {
        pendingMemory->writed(pendingGuest, n);
        pendingWritten = n;
    }
}

}

extern "C" {

uint32_t wd3d_arg(uint32_t n) {
    return callCpu->peek32(n);
}

uint32_t wd3d_read32(uint32_t guest) {
    return callCpu->memory->readd(guest);
}

void wd3d_write32(uint32_t guest, uint32_t value) {
    callCpu->memory->writed(guest, value);
}

void wd3d_read(uint32_t guest, void* dst, uint32_t len) {
    if (len) callCpu->memory->memcpy(dst, guest, len);
}

void wd3d_write(uint32_t guest, const void* src, uint32_t len) {
    if (len) callCpu->memory->memcpy(guest, src, len);
}

void* wd3d_tmp(uint32_t len) {
    void* p = calloc(1, len ? len : 1);
    if (!p) kpanic_fmt("host wined3d: out of memory for a %u byte call buffer", len);
    callTmp->push_back(p);
    return p;
}

void* wd3d_read_array(uint32_t guest, uint32_t count, uint32_t size) {
    if (count > MAX_ARRAY_COUNT) {
        klog_fmt("[wined3d] array of %u elements at %.8X capped at %u", count, guest, MAX_ARRAY_COUNT);
        count = MAX_ARRAY_COUNT;
    }
    void* p = wd3d_tmp(count * size);
    wd3d_read(guest, p, count * size);
    return p;
}

const char* wd3d_read_string(uint32_t guest) {
    if (!guest) return nullptr;
    U32 len = (U32)callCpu->memory->strlen(guest);
    char* p = (char*)wd3d_tmp(len + 1);
    wd3d_read(guest, p, len);
    return p;
}

uint32_t wd3d_map_native(void* host, uint32_t len) {
    return callCpu->memory->mapNativeMemory(host, len);
}

void wd3d_unmap_native(uint32_t guest, uint32_t len) {
    callCpu->memory->unmapNativeMemory(guest, len);
}

uint32_t wd3d_not_forwarded(uint32_t id, uint32_t ret) {
    if (id < WD3D_ID_COUNT && !loggedOnce[id]) {
        loggedOnce[id] = true;
        klog_fmt("[wined3d] %s is not forwarded to host wined3d yet: answered %.8X", wd3d_call_names[id] ? wd3d_call_names[id] : "?", ret);
    }
    return ret;
}

}

void hostWined3dStatsLine(char* out, int len, double sec) {
    U32 total = 0;
    double ms = 0;
    U32 top[8] = {};
    for (U32 i = 0; i < WD3D_ID_COUNT; i++) {
        total += statCalls[i];
        ms += statMs[i];
        for (int k = 0; k < 8; k++) {
            if (statMs[i] > statMs[top[k]]) {
                memmove(top + k + 1, top + k, (7 - k) * sizeof(U32));
                top[k] = i;
                break;
            }
        }
    }
    int n = snprintf(out, len, " | wd3d calls/s=%.0f ms/s=%.0f top:", total / sec, ms / sec);
    for (int k = 0; k < 8 && n < len && statCalls[top[k]]; k++) {
        U32 i = top[k];
        n += snprintf(out + n, len - n, " %s %.0f/s %.1fms/s", i < 16 ? "ctl" : (wd3d_call_names[i] ? wd3d_call_names[i] : "?"), statCalls[i] / sec, statMs[i] / sec);
    }
    memset(statCalls, 0, sizeof(statCalls));
    memset(statMs, 0, sizeof(statMs));
}

static void hostWined3dCallInner(CPU* cpu, U32 index);

void hostWined3dCall(CPU* cpu, U32 index) {
#ifdef __EMSCRIPTEN__
    if (vnboxStats.on) {
        U32 id = index - WD3D_CALL_BASE;
        double start = emscripten_get_now();
        hostWined3dCallInner(cpu, index);
        if (id < WD3D_ID_COUNT) {
            statCalls[id]++;
            statMs[id] += emscripten_get_now() - start;
        }
        return;
    }
#endif
    hostWined3dCallInner(cpu, index);
}

static void hostWined3dCallInner(CPU* cpu, U32 index) {
    U32 id = index - WD3D_CALL_BASE;
    CallScope scope(cpu);
    U32 result = 0;
    if (id == WD3D_ID_HELLO) {
        result = hello(wd3d_arg(1), wd3d_arg(2));
    } else if (id == WD3D_ID_REGISTER_WINDOW) {
#ifdef BOXEDWINE_OPENGL
        // (hwnd, X drawable, client x, y, width, height, visible, client x, y in the window rect, window width, height)
        hostWined3dRegisterWindow(wd3d_arg(1), wd3d_arg(2), (S32)wd3d_arg(3), (S32)wd3d_arg(4), (S32)wd3d_arg(5), (S32)wd3d_arg(6), wd3d_arg(7),
                (S32)wd3d_arg(8), (S32)wd3d_arg(9), (S32)wd3d_arg(10), (S32)wd3d_arg(11));
        result = 1;
#endif
    } else if (id >= 16 && id < WD3D_ID_COUNT) {
        // A guest that skipped DllMain (or whose hello failed) still gets an attached wined3d or a 0.
        if (attach()) {
#ifdef BOXEDWINE_OPENGL
            struct GlScope { // RAII: a guest fault unwinds through here (CallScope)
                uint32_t saved[3];
                int switched = hostwd3d_gl_enter(saved);
                ~GlScope() { if (switched) hostwd3d_gl_leave(saved); }
            } gl;
#endif
            result = wd3d_dispatch(id);
        }
    } else if (id == WD3D_ID_DRAIN_CALLBACKS) {
        // (struct hostwd3d_callback *out, max) -> how many were moved; the stub repeats until it gets 0, making
        // each call before asking again (a callback may call wined3d and queue more).
        U32 out = wd3d_arg(1), max = std::min(wd3d_arg(2), 64u);
        hostwd3d_callback cbs[64];
        result = hostwd3d_callbacks_drain(cbs, out ? max : 0);
        wd3d_write(out, cbs, result * sizeof(hostwd3d_callback));
    } else {
        klog_fmt("[wined3d] unknown thunk call %u", id);
    }
    updatePending();
    cpu->reg[0].u32 = result; // EAX
}

#endif
