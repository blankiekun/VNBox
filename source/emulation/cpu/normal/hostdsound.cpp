/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "boxedwine.h"
#include "vnboxparam.h"

// Host dsound mixing: every game that plays audio runs Wine's dsound mixer thread, and its per-sample loops (each
// playing buffer's 16-bit/float samples to floats, volume, the add into the primary buffer) were emulated x86 on every
// mix period. jp.zip's dsound.dll.so (vn/wine/dsound-mixer-fast.patch) puts those loops in two exported leaf functions,
// vnbox_dsound_convert and vnbox_dsound_mix, which get a head op here (hostLibcOp, the native libc mechanism in
// hosthooks.cpp) and run as host code. Same bytes: the guest does single float operations (SSE, -mfpmath=sse), each
// rounded to float, and the emulator does them as host float operations, which is what this code does, in the same
// order and with the same operands (16-bit samples / 32768 is exact). Nothing about buffers or timing changes: the
// mixer calls the same functions at the same moments, they only take less emulated time.
// A call whose memory isn't plain RAM, whose ranges overlap (the copies would not see each other's writes), or whose
// arguments are outside what the patched dsound passes runs the guest function. The root fs's dsound.dll.so (no
// exports) gets nothing hooked. nativeDsound=0 (URL) turns it off.
#include "../../softmmu/kmemory_soft.h"

enum : U32 { DS_CONVERT, DS_MIX, DS_COUNT };
static const char* const hostDsoundNames[DS_COUNT] = { "vnbox_dsound_convert", "vnbox_dsound_mix" };

U32 hostDsoundFnId(const char* name) { // DS_* + 1, or 0
    for (U32 i = 0; i < DS_COUNT; i++) {
        if (!strcmp(hostDsoundNames[i], name)) return i + 1;
    }
    return 0;
}

// Whether dsound.dll.so's exports get hooked at all (read once, when it is first mapped).
bool hostDsoundEnabled() {
#ifdef __EMSCRIPTEN__
    if (vnboxUrlIs("nativeDsound", "0")) {
        klog_fmt("[vnbox] native dsound off (nativeDsound=0)");
        return false;
    }
#endif
    return true;
}

bool libcRange(KMemory* memory, U32 address, U32 len, bool write); // hosthooks.cpp: plain RAM, no wrap

static const U64 DS_MAX_BYTES = 1u << 24; // far above a mix period (the patched dsound mixes at most 3 fragments)

static bool dsOverlap(U64 a, U64 aLen, U64 b, U64 bLen) {
    return aLen && bLen && a < b + bLen && b < a + aLen;
}

// Runs vnbox_dsound_* `fn` (DS_*) for the call the CPU is at (i386 cdecl, void) and returns like its `ret`; false
// (nothing changed) leaves it to the guest function.
bool hostDsoundRun(CPU* cpu, U32 fn) {
    KMemory* memory = cpu->memory;
    if (fn >= DS_COUNT || cpu->debugTrapActive || !cpu->isBig() || cpu->stackMask != 0xFFFFFFFF ||
        cpu->seg[CS].address || cpu->seg[DS].address || cpu->seg[ES].address || cpu->seg[SS].address ||
        !libcRange(memory, ESP, 4 + 8 * 4, false)) return false;
    U32 a[8];
    for (U32 i = 0; i < 8; i++) a[i] = memory->readd(ESP + 4 + i * 4);
    static thread_local std::vector<U8> in, out;
    if (fn == DS_CONVERT) { // (p, istride, out, ochannels, n, channels, s16, dup)
        U32 p = a[0], istride = a[1], o = a[2], ochannels = a[3], n = a[4], channels = a[5];
        bool s16 = a[6] != 0, dup = a[7] != 0;
        U32 width = dup ? 2 : channels; // floats written per frame
        if (!channels || channels > 32 || (dup && channels != 1) || ochannels < width) return false;
        if (n) {
            U32 sample = s16 ? 2 : 4;
            U64 inLen = (U64)(n - 1) * istride + (U64)channels * sample, outLen = ((U64)(n - 1) * ochannels + width) * 4;
            if (inLen > DS_MAX_BYTES || outLen > DS_MAX_BYTES || dsOverlap(p, inLen, o, outLen) ||
                !libcRange(memory, p, (U32)inLen, false) || !libcRange(memory, o, (U32)outLen, true)) return false;
            in.resize((size_t)inLen);
            out.resize((size_t)outLen);
            memory->memcpy(in.data(), p, (U32)inLen);
            memory->memcpy(out.data(), o, (U32)outLen); // bytes between the frames' values stay as they were
            const U8* from = in.data();
            U8* to = out.data();
            for (U32 f = 0; f < n; f++, from += istride, to += ochannels * 4) {
                for (U32 c = 0; c < channels; c++) {
                    if (s16) {
                        S16 v;
                        ::memcpy(&v, from + c * 2, 2);
                        float x = v / (float)0x8000;
                        ::memcpy(to + c * 4, &x, 4);
                    } else {
                        ::memcpy(to + c * 4, from + c * 4, 4); // a float load and store: the bits as they are
                    }
                }
                if (dup) ::memcpy(to + 4, to, 4);
            }
            memory->memcpy(o, out.data(), (U32)outLen);
        }
    } else { // DS_MIX (src, dst, frames, channels, vols): src *= vols per channel when vols, then dst += src
        U32 src = a[0], dst = a[1], frames = a[2], channels = a[3], vols = a[4];
        U64 samples = (U64)frames * channels, len = samples * 4;
        if (len > DS_MAX_BYTES || (vols && channels > 32)) return false;
        if (len) {
            U32 volsLen = vols ? channels * 4 : 0;
            if (dsOverlap(src, len, dst, len) || dsOverlap(src, len, vols, volsLen) || dsOverlap(dst, len, vols, volsLen) ||
                !libcRange(memory, src, (U32)len, vols != 0) || !libcRange(memory, dst, (U32)len, true) ||
                (vols && !libcRange(memory, vols, volsLen, false))) return false;
            float v[32];
            if (vols) memory->memcpy(v, vols, volsLen);
            in.resize((size_t)len);
            out.resize((size_t)len);
            memory->memcpy(in.data(), src, (U32)len);
            memory->memcpy(out.data(), dst, (U32)len);
            float* s = (float*)in.data();
            float* d = (float*)out.data();
            if (vols) {
                for (U32 i = 0; i < frames; i++) {
                    for (U32 c = 0; c < channels; c++) s[i * channels + c] = s[i * channels + c] * v[c];
                }
                memory->memcpy(src, in.data(), (U32)len);
            }
            for (U32 i = 0; i < (U32)samples; i++) d[i] = d[i] + s[i];
            memory->memcpy(dst, out.data(), (U32)len);
        }
    }
    cpu->eip.u32 = cpu->pop32();
    return true;
}
