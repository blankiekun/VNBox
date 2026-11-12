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
// mix period. jp.zip's dsound.dll.so (vn/wine/dsound-mixer-fast.patch) puts those loops in exported leaf functions,
// vnbox_dsound_convert, vnbox_dsound_mix and vnbox_dsound_resample (the FIR resampler, for a buffer whose rate is not
// the device's), which get a head op here (hostLibcOp, the native libc mechanism in hosthooks.cpp) and run as host
// code. Same bytes: the guest does single float/double operations (SSE, -mfpmath=sse), each rounded to its type, and
// the emulator does them as host operations, which is what this code does, in the same order and with the same
// operands and types (16-bit samples / 32768 is exact). Nothing about buffers or timing changes: the
// mixer calls the same functions at the same moments, they only take less emulated time.
// A call whose memory isn't plain RAM, whose ranges overlap (the copies would not see each other's writes), or whose
// arguments are outside what the patched dsound passes runs the guest function. The root fs's dsound.dll.so (no
// exports) gets nothing hooked. nativeDsound=0 (URL) turns it off.
#include "../../softmmu/kmemory_soft.h"

enum : U32 { DS_CONVERT, DS_MIX, DS_RESAMPLE, DS_COUNT };
static const char* const hostDsoundNames[DS_COUNT] = { "vnbox_dsound_convert", "vnbox_dsound_mix", "vnbox_dsound_resample" };

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
        !libcRange(memory, ESP, 4 + (fn == DS_RESAMPLE ? 18 : 8) * 4, false)) return false;
    U32 a[18];
    for (U32 i = 0; i < (fn == DS_RESAMPLE ? 18u : 8u); i++) a[i] = memory->readd(ESP + 4 + i * 4);
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
    } else if (fn == DS_RESAMPLE) {
        // (out, ochannels, count, freqAcc_start, freqAdjustNum, freqAdjustDen (LONG64s, two slots each), firstep,
        // firgain, fir, fir_len, fir_copy, intermediate, required_input, channels, dup): cp_fields_resample's loop
        U32 o = a[0], ochannels = a[1], count = a[2], firstep = a[9], fir = a[11], firLen = a[12], firCopy = a[13];
        U32 inter = a[14], required = a[15], channels = a[16];
        S64 start = (S64)(a[3] | (U64)a[4] << 32), num = (S64)(a[5] | (U64)a[6] << 32), den = (S64)(a[7] | (U64)a[8] << 32);
        float firgain;
        ::memcpy(&firgain, &a[10], 4);
        bool dup = a[17] != 0;
        U32 width = dup ? 2 : channels;
        if (!channels || channels > 32 || (dup && channels != 1) || ochannels < width || !firstep || den <= 0 ||
            firLen < 2) return false;
        if (count) {
            U32 maxUsed = (firLen - 2) / firstep + 1; // fir_copy entries a frame can write (its first idx is >= 0)
            U64 outLen = ((U64)(count - 1) * ochannels + width) * 4, firBytes = (U64)firLen * 4, copyLen = (U64)maxUsed * 4;
            U64 interLen = (U64)channels * required * 4;
            if (outLen > DS_MAX_BYTES || firBytes > DS_MAX_BYTES || interLen > DS_MAX_BYTES ||
                dsOverlap(o, outLen, fir, firBytes) || dsOverlap(o, outLen, firCopy, copyLen) ||
                dsOverlap(o, outLen, inter, interLen) || dsOverlap(firCopy, copyLen, fir, firBytes) ||
                dsOverlap(firCopy, copyLen, inter, interLen) || !libcRange(memory, o, (U32)outLen, true) ||
                !libcRange(memory, fir, (U32)firBytes, false) || !libcRange(memory, firCopy, (U32)copyLen, true) ||
                !libcRange(memory, inter, (U32)interLen, false)) return false;
            static thread_local std::vector<float> f, fc, cache;
            f.resize(firLen);
            fc.resize(maxUsed);
            cache.resize((size_t)channels * required);
            out.resize((size_t)outLen);
            memory->memcpy(f.data(), fir, (U32)firBytes);
            memory->memcpy(fc.data(), firCopy, (U32)copyLen); // entries no frame writes stay as they were
            if (interLen) memory->memcpy(cache.data(), inter, (U32)interLen);
            memory->memcpy(out.data(), o, (U32)outLen); // bytes between the frames' values stay as they were
            float denF = (float)den;
            U8* to = out.data();
            // The guest's expressions with the guest's types: LONG64 math (wrapping, done as U64), the LONG64 to float
            // conversion and float divide, rem in double rounded to float, a float times float product added in double.
            for (U32 i = 0; i < count; i++, to += ochannels * 4) {
                S64 steps = (S64)(((U64)start + (U64)i * (U64)num) * firstep);
                U32 intFirSteps = (U32)(steps / den);
                float totalFirSteps = steps / denF;
                U32 ipos = intFirSteps / firstep;
                U32 idx = (ipos + 1) * firstep - intFirSteps - 1;
                float rem = intFirSteps + 1.0 - totalFirSteps;
                U32 used = 0;
                while (idx < firLen - 1) {
                    if (used == maxUsed) return false; // not reachable (idx wraps only with a huge firstep)
                    fc[used++] = f[idx] * (1.0 - rem) + f[idx + 1] * rem;
                    idx += firstep;
                }
                if (used && (U64)ipos + used > required) return false; // the guest would read past its row
                for (U32 c = 0; c < channels; c++) {
                    float sum = 0.0;
                    const float* row = cache.data() + (size_t)c * required + ipos;
                    for (U32 j = 0; j < used; j++) sum += fc[j] * row[j];
                    float v = sum * firgain;
                    ::memcpy(to + c * 4, &v, 4);
                }
                if (dup) ::memcpy(to + 4, to, 4);
            }
            memory->memcpy(firCopy, fc.data(), (U32)copyLen);
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
