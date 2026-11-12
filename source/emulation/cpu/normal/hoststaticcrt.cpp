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

// Statically linked MSVC CRT: a game exe (or DLL) built with /MT carries its own memcpy, memset, strlen and strncmp,
// with no export or symbol naming them, so the name-based tables in hosthooks.cpp can't see them. Here a block start is
// recognised by its bytes instead: when the code at a block's first address matches one of the known CRT bodies below
// (relocated addresses and calls out of the function masked), the block gets the native libc head op (hostLibcOp) and the
// call runs as hostLibcRun's existing case for that function, with all of its fallbacks (memory that isn't plain RAM, DF
// set, debug traps, segments) going back to the guest body as before.
// - Only a block starting exactly at a signature's first byte matches, so only whole functions entered at their start.
// - The match is made again on every call (hostStaticCrtFn), so a write anywhere in the signature, not only in the head's
//   first instruction (whose page write invalidates the decoded block), sends that call to the guest. Unpacked or
//   self-modified code is matched as it is when the block is decoded.
// - The bodies are cdecl leaf functions: arguments on the stack, result in EAX, and the caller assumes ECX, EDX and the
//   flags are clobbered, which the head op leaves as they were. MSVC's memcpy is the same code as its memmove (it copies
//   backward when dst is inside [src, src + n)), so both go to the memmove case.
// nativeStaticCrt=0 or nativeLibc=0 (URL) turns it off.
#include "../../softmmu/kmemory_soft.h"
#include "../../softmmu/soft_ram.h"

enum : U8 { SC_NONE, SC_MEMMOVE, SC_MEMSET, SC_STRLEN, SC_STRNCMP };

struct HostCrtSig {
    const char* what; // function and the MSVC family it was seen in (the exe's linker version)
    U8 kind; // SC_*
    U8 len;
    U8 skip[2][2]; // masked [offset, length]: a relocated address or a rel32 to another function (zero in bytes)
    const U8* bytes;
};

// Prefixes of the CRT bodies as they appear in exes built by these linkers, from MSVC's own crt/src/intel *.asm shapes
// (memcpy.asm, memset.asm, strlen.asm) and the C strncmp of VC8. Each prefix covers the entry, the argument loads and
// the first dispatch, which is what tells these functions apart from anything else.
static const U8 scMemcpy6[] = { // VC6: push ebp; mov ebp, esp; ... overlap test, rep movsd, jmp [table]
    0x55, 0x8b, 0xec, 0x57, 0x56, 0x8b, 0x75, 0x0c, 0x8b, 0x4d, 0x10, 0x8b, 0x7d, 0x08, 0x8b, 0xc1, 0x8b, 0xd1, 0x03, 0xc6,
    0x3b, 0xfe, 0x76, 0x08, 0x3b, 0xf8, 0x0f, 0x82, 0x78, 0x01, 0x00, 0x00, 0xf7, 0xc7, 0x03, 0x00, 0x00, 0x00, 0x75, 0x14,
    0xc1, 0xe9, 0x02, 0x83, 0xe2, 0x03, 0x83, 0xf9, 0x08, 0x72, 0x29, 0xf3, 0xa5, 0xff, 0x24, 0x95, 0, 0, 0, 0
};
static const U8 scMemcpy8[] = { // VC8: as VC6 plus the SSE2 fast copy test (__sse2_available, jmp _VEC_memcpy)
    0x55, 0x8b, 0xec, 0x57, 0x56, 0x8b, 0x75, 0x0c, 0x8b, 0x4d, 0x10, 0x8b, 0x7d, 0x08, 0x8b, 0xc1, 0x8b, 0xd1, 0x03, 0xc6,
    0x3b, 0xfe, 0x76, 0x08, 0x3b, 0xf8, 0x0f, 0x82, 0xa4, 0x01, 0x00, 0x00, 0x81, 0xf9, 0x00, 0x01, 0x00, 0x00, 0x72, 0x1f,
    0x83, 0x3d, 0, 0, 0, 0, 0x00, 0x74, 0x16, 0x57, 0x56, 0x83, 0xe7, 0x0f, 0x83, 0xe6, 0x0f, 0x3b, 0xfe, 0x5e, 0x5f, 0x75,
    0x08, 0x5e, 0x5f, 0x5d, 0xe9, 0, 0, 0, 0
};
static const U8 scMemcpy14[] = { // VC14 (VS2015+): no frame, size classes, bt __favor / __isa_enabled
    0x57, 0x56, 0x8b, 0x74, 0x24, 0x10, 0x8b, 0x4c, 0x24, 0x14, 0x8b, 0x7c, 0x24, 0x0c, 0x8b, 0xc1, 0x8b, 0xd1, 0x03, 0xc6,
    0x3b, 0xfe, 0x76, 0x08, 0x3b, 0xf8, 0x0f, 0x82, 0x94, 0x02, 0x00, 0x00, 0x83, 0xf9, 0x20, 0x0f, 0x82, 0xd2, 0x04, 0x00,
    0x00, 0x81, 0xf9, 0x80, 0x00, 0x00, 0x00, 0x73, 0x13, 0x0f, 0xba, 0x25, 0, 0, 0, 0, 0x01, 0x0f, 0x82, 0x8e, 0x04, 0x00,
    0x00, 0xe9, 0xe3, 0x01, 0x00, 0x00
};
static const U8 scMemset6[] = { // VC6: byte head, splat, rep stosd, byte tail
    0x8b, 0x54, 0x24, 0x0c, 0x8b, 0x4c, 0x24, 0x04, 0x85, 0xd2, 0x74, 0x47, 0x33, 0xc0, 0x8a, 0x44, 0x24, 0x08, 0x57, 0x8b,
    0xf9, 0x83, 0xfa, 0x04, 0x72, 0x2d, 0xf7, 0xd9, 0x83, 0xe1, 0x03, 0x74, 0x08, 0x2b, 0xd1, 0x88, 0x07, 0x47, 0x49, 0x75,
    0xfa, 0x8b, 0xc8, 0xc1, 0xe0, 0x08, 0x03, 0xc1, 0x8b, 0xc8, 0xc1, 0xe0, 0x10, 0x03, 0xc1, 0x8b, 0xca, 0x83, 0xe2, 0x03,
    0xc1, 0xe9, 0x02, 0x74
};
static const U8 scMemset8[] = { // VC8: as VC6 plus the SSE2 zero fill test (__sse2_available, jmp _VEC_memzero)
    0x8b, 0x54, 0x24, 0x0c, 0x8b, 0x4c, 0x24, 0x04, 0x85, 0xd2, 0x74, 0x69, 0x33, 0xc0, 0x8a, 0x44, 0x24, 0x08, 0x84, 0xc0,
    0x75, 0x16, 0x81, 0xfa, 0x00, 0x01, 0x00, 0x00, 0x72, 0x0e, 0x83, 0x3d, 0, 0, 0, 0, 0x00, 0x74, 0x05, 0xe9,
    0, 0, 0, 0, 0x57, 0x8b, 0xf9, 0x83, 0xfa, 0x04, 0x72, 0x31, 0xf7, 0xd9, 0x83, 0xe1, 0x03, 0x74, 0x0c, 0x2b,
    0xd1, 0x88, 0x07, 0x83
};
static const U8 scMemset14[] = { // VC14: imul splat, size classes, bt __favor
    0x8b, 0x4c, 0x24, 0x0c, 0x0f, 0xb6, 0x44, 0x24, 0x08, 0x8b, 0xd7, 0x8b, 0x7c, 0x24, 0x04, 0x85, 0xc9, 0x0f, 0x84, 0x3c,
    0x01, 0x00, 0x00, 0x69, 0xc0, 0x01, 0x01, 0x01, 0x01, 0x83, 0xf9, 0x20, 0x0f, 0x8e, 0xdf, 0x00, 0x00, 0x00, 0x81, 0xf9,
    0x80, 0x00, 0x00, 0x00, 0x0f, 0x8c, 0x8b, 0x00, 0x00, 0x00, 0x0f, 0xba, 0x25, 0, 0, 0, 0, 0x01
};
static const U8 scStrlen6[] = { // VC6: byte loop to alignment, then the 0x7efefeff word scan
    0x8b, 0x4c, 0x24, 0x04, 0xf7, 0xc1, 0x03, 0x00, 0x00, 0x00, 0x74, 0x14, 0x8a, 0x01, 0x41, 0x84, 0xc0, 0x74, 0x40, 0xf7,
    0xc1, 0x03, 0x00, 0x00, 0x00, 0x75, 0xf1, 0x05, 0x00, 0x00, 0x00, 0x00, 0x8b, 0x01, 0xba, 0xff, 0xfe, 0xfe, 0x7e, 0x03,
    0xd0, 0x83, 0xf0, 0xff, 0x33, 0xc2, 0x83, 0xc1, 0x04, 0xa9, 0x00, 0x01, 0x01, 0x81, 0x74, 0xe8, 0x8b, 0x41, 0xfc, 0x84,
    0xc0, 0x74, 0x32, 0x84
};
static const U8 scStrlen8[] = { // VC8: the same with add ecx, 1 and aligned padding
    0x8b, 0x4c, 0x24, 0x04, 0xf7, 0xc1, 0x03, 0x00, 0x00, 0x00, 0x74, 0x24, 0x8a, 0x01, 0x83, 0xc1, 0x01, 0x84, 0xc0, 0x74,
    0x4e, 0xf7, 0xc1, 0x03, 0x00, 0x00, 0x00, 0x75, 0xef, 0x05, 0x00, 0x00, 0x00, 0x00, 0x8d, 0xa4, 0x24, 0x00, 0x00, 0x00,
    0x00, 0x8d, 0xa4, 0x24, 0x00, 0x00, 0x00, 0x00, 0x8b, 0x01, 0xba, 0xff, 0xfe, 0xfe, 0x7e, 0x03, 0xd0, 0x83, 0xf0, 0xff,
    0x33, 0xc2, 0x83, 0xc1
};
static const U8 scStrncmp8[] = { // VC8: C strncmp, 4x unrolled; returns the unsigned byte difference
    0x55, 0x8b, 0xec, 0x51, 0x83, 0x65, 0xfc, 0x00, 0x53, 0x8b, 0x5d, 0x10, 0x85, 0xdb, 0x75, 0x07, 0x33, 0xc0, 0xe9, 0x9a,
    0x00, 0x00, 0x00, 0x83, 0xfb, 0x04, 0x57, 0x72, 0x75, 0x8d, 0x7b, 0xfc, 0x85, 0xff, 0x76, 0x6e, 0x8b, 0x4d, 0x0c, 0x8b,
    0x45, 0x08, 0x8a, 0x10, 0x83, 0xc0, 0x04, 0x83, 0xc1, 0x04, 0x84, 0xd2, 0x74, 0x52, 0x3a, 0x51, 0xfc, 0x75, 0x4d, 0x8a,
    0x50, 0xfd, 0x84, 0xd2
};

static const HostCrtSig hostCrtSigs[] = {
    { "memcpy/memmove VC6", SC_MEMMOVE, sizeof(scMemcpy6), { { 0x38, 4 }, { 0, 0 } }, scMemcpy6 },
    { "memcpy/memmove VC8", SC_MEMMOVE, sizeof(scMemcpy8), { { 0x2a, 4 }, { 0x43, 4 } }, scMemcpy8 },
    { "memcpy/memmove VC14", SC_MEMMOVE, sizeof(scMemcpy14), { { 0x34, 4 }, { 0, 0 } }, scMemcpy14 },
    { "memset VC6", SC_MEMSET, sizeof(scMemset6), { { 0, 0 }, { 0, 0 } }, scMemset6 },
    { "memset VC8", SC_MEMSET, sizeof(scMemset8), { { 0x20, 4 }, { 0x28, 4 } }, scMemset8 },
    { "memset VC14", SC_MEMSET, sizeof(scMemset14), { { 0x35, 4 }, { 0, 0 } }, scMemset14 },
    { "strlen VC6", SC_STRLEN, sizeof(scStrlen6), { { 0, 0 }, { 0, 0 } }, scStrlen6 },
    { "strlen VC8", SC_STRLEN, sizeof(scStrlen8), { { 0, 0 }, { 0, 0 } }, scStrlen8 },
    { "strncmp VC8", SC_STRNCMP, sizeof(scStrncmp8), { { 0, 0 }, { 0, 0 } }, scStrncmp8 },
};
static const U32 hostCrtSigCount = sizeof(hostCrtSigs) / sizeof(hostCrtSigs[0]);
static const U32 hostCrtSigMax = 0x48;

static int hostStaticCrtState; // 0: not decided yet, 1: on, -1: off

#ifdef __TEST
bool hostStaticCrtTestOff; // testHostStaticCrt.cpp
#endif

static bool hostStaticCrtEnabled() {
#ifdef __TEST
    if (hostStaticCrtTestOff) return false;
#endif
    if (!hostStaticCrtState) {
        hostStaticCrtState = 1;
#ifdef __EMSCRIPTEN__
        if (vnboxUrlIs("nativeStaticCrt", "0") || vnboxUrlIs("nativeLibc", "0")) {
            klog_fmt("[vnbox] static CRT matching off");
            hostStaticCrtState = -1;
        }
#endif
    }
    return hostStaticCrtState > 0;
}

// Guest code bytes that are already RAM; never loads a page (this runs while a block is decoded).
static bool scPeek(KMemory* memory, U32 address, U8* out, U32 len) {
    while (len) {
        if (address >> K_PAGE_SHIFT >= K_NUMBER_OF_PAGES) return false;
        MMU& mmu = getMemData(memory)->mmu[address >> K_PAGE_SHIFT];
        if (!mmu.canReadRam) return false;
        U32 n = std::min(len, K_PAGE_SIZE - (address & K_PAGE_MASK));
        ::memcpy(out, ramPageGet((RamPage)mmu.ramIndex) + (address & K_PAGE_MASK), n);
        out += n;
        address += n;
        len -= n;
        if (!address && len) return false; // wrapped
    }
    return true;
}

static bool scMatches(const HostCrtSig& sig, const U8* code) {
    U32 at = 0;
    for (const auto& skip : sig.skip) {
        if (!skip[1]) continue;
        if (memcmp(code + at, sig.bytes + at, skip[0] - at)) return false;
        at = skip[0] + skip[1];
    }
    return !memcmp(code + at, sig.bytes + at, sig.len - at);
}

// Signature index + 1 when a known CRT body starts at eip (linear), else 0. Called when a block is decoded.
U32 hostStaticCrtAt(CPU* cpu, U32 eip) {
    U8 code[hostCrtSigMax];
    if (!scPeek(cpu->memory, eip, code, 4) || !hostStaticCrtEnabled()) return 0;
    bool peeked = false;
    for (U32 i = 0; i < hostCrtSigCount; i++) {
        const HostCrtSig& sig = hostCrtSigs[i];
        if (memcmp(code, sig.bytes, 4)) continue;
        if (!peeked) {
            if (!scPeek(cpu->memory, eip, code, hostCrtSigMax)) return 0;
            peeked = true;
        }
        if (scMatches(sig, code)) return i + 1;
    }
    return 0;
}

// SC_* of signature `index` (from hostStaticCrtAt) if the bytes at the CPU's eip still match it, else SC_NONE.
U32 hostStaticCrtFn(CPU* cpu, U32 index) {
    if (index >= hostCrtSigCount || !hostStaticCrtEnabled()) return SC_NONE;
    const HostCrtSig& sig = hostCrtSigs[index];
    U8 code[hostCrtSigMax];
    if (!scPeek(cpu->memory, cpu->seg[CS].address + cpu->eip.u32, code, sig.len) || !scMatches(sig, code)) return SC_NONE;
    return sig.kind;
}

#ifdef __TEST
// testHostStaticCrt.cpp: the signature table, to place a matching body in guest memory.
const U8* hostStaticCrtTestSig(U32 index, U32& len, U32& kind) {
    if (index >= hostCrtSigCount) return nullptr;
    len = hostCrtSigs[index].len;
    kind = hostCrtSigs[index].kind;
    return hostCrtSigs[index].bytes;
}

bool hostStaticCrtTestMasked(U32 index, U32 offset) {
    for (const auto& skip : hostCrtSigs[index].skip) {
        if (offset >= skip[0] && offset < (U32)skip[0] + skip[1]) return true;
    }
    return false;
}
#endif
