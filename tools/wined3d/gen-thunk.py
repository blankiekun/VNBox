#!/usr/bin/env python3
"""Generates both sides of the host wined3d thunk from wined3d.h (+ thunk_rules.py):

  source/wined3d/thunk_ids.h        call ids, shared by both sides, and the protocol hash
  source/wined3d/thunk_dispatch.c   host: wd3d_dispatch(id) reads the guest's cdecl args, calls host wined3d
  source/wined3d/guest/wined3d_thunk.S, wined3d.def
                                     guest: the stub wined3d.dll, one export per wined3d_* function, each
                                     `int $0x99; .long WD3D_CALL_BASE + id; ret` (Boxedwine's int 0x99 host call)

Usage: tools/wined3d/gen-thunk.py <wined3d.h>   (writes into the repo; --check only compares)
Struct layouts are the same for the guest (i386 ELF and MinGW) and the host (wasm32), checked with clang's record
layout dump (all 61 complete wined3d structs; only wined3d_dvec4's alignment differs, not its offsets), so structs
cross as raw bytes; pointers inside them are the rules' business.
"""
import os, re, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import wd3dproto as w
import thunk_rules as R

REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
CALL_BASE = 0x00F00000  # int 0x99 index range for wined3d calls (GL uses small indices; JIT fast path <= 0xffffff)
FIRST_ID = 16           # 0 = hello (protocol check), 1 = window registration, 2 = callback drain, 3 = DC info, ..15 reserved
MAX_COUNT = 65536

SCALAR_POINTEES = {'unsigned int', 'UINT', 'BOOL', 'DWORD', 'RECT', 'GUID', 'HDC', 'float', 'uint32_t', 'int', 'LONG'}

class Unsupported(Exception):
    pass

def is_enum(t):
    return t.startswith('enum ')

def gen_function(p, complete, rules_arrays):
    """C statements for one forwarded function: returns (lines, ret_expr_assign)."""
    pre, call_args, post, fail = [], [], [], []
    slot = 1  # peek32(0) is the return address
    argvar = {}
    for a in p.params:
        argvar[a.name] = f'a{slot}'
        if a.ptr == 0 and a.base in ('UINT64', 'uint64_t', 'ULONGLONG', 'LONGLONG', 'double', 'INT64', 'SIZE_T64'):
            raise Unsupported(f'64-bit argument {a.text}')
        if a.ptr == 0 and a.base.startswith('struct '):
            raise Unsupported(f'struct by value {a.text}')
        pre.append(f'uint32_t a{slot} = wd3d_arg({slot});')
        slot += 1

    def count_expr(param):
        rule = rules_arrays.get((p.name, param.name))
        if rule is None:
            if param.array:
                raise Unsupported(f'array without a count: {param.text}')
            return None
        if rule.startswith('*'):
            v = argvar[rule[1:]]
            return f'({v} ? wd3d_read32({v}) : 0)'
        if rule in argvar:
            return argvar[rule]
        return rule

    for a in p.params:
        v = argvar[a.name]
        t = a.type
        b = a.base
        sb = b[len('struct '):] if b.startswith('struct ') else None
        if a.ptr == 0:
            if b == 'float':
                call_args.append(f'wd3d_f32({v})')
            elif b in ('bool', 'BYTE', 'BOOLEAN'):
                call_args.append(f'({b}){v}')
            else:
                call_args.append(f'({t})(uintptr_t){v}')
            continue
        if sb and sb in R.CALLBACK_STRUCTS:
            wrap, free = R.CALLBACK_STRUCTS[sb]
            pre.append(f'{t} w{v} = {v} ? ({t}){wrap}({v}) : NULL;')
            call_args.append(f'w{v}')
            fail.append(f'{free}(w{v});')
            continue
        if sb == 'wined3d_parent_ops':
            if 'parent' not in argvar:
                raise Unsupported(f'parent_ops without a parent: {a.text}')
            call_args.append(f'({t}){R.PARENT_OPS}()')
            continue
        if sb and sb not in complete:  # opaque wined3d object
            if a.ptr == 1:
                call_args.append(f'({t})(uintptr_t){v}')
            elif '*const*' in t.replace(' ', ''):  # array of handles in
                n = count_expr(a)
                if n is None:
                    raise Unsupported(f'handle array without a count: {a.text}')
                pre.append(f'{t} h{v} = {v} ? ({t})wd3d_read_array({v}, {n}, 4) : NULL;')
                call_args.append(f'h{v}')
            elif a.ptr == 2:  # handle out (read first: in/out is harmless)
                inner = t[:-1]
                pre.append(f'{inner} h{v} = {v} ? ({inner})(uintptr_t)wd3d_read32({v}) : NULL;')
                call_args.append(f'{v} ? &h{v} : NULL')
                post.append(f'if ({v}) wd3d_write32({v}, (uint32_t)(uintptr_t)h{v});')
            else:
                raise Unsupported(f'pointer depth {a.text}')
            continue
        if a.ptr != 1:
            raise Unsupported(f'pointer depth {a.text}')
        if b == 'void' and a.name == 'parent':  # the guest's object: wrapped with its parent_ops
            ops = next((x for x in p.params if x.base == 'struct wined3d_parent_ops'), None)
            if ops is None:
                raise Unsupported(f'parent without parent_ops: {a.text}')
            pre.append(f'void *w{v} = {R.PARENT_WRAP}({v}, {argvar[ops.name]});')
            call_args.append(f'w{v}')
            fail.append(f'{R.PARENT_FREE}(w{v});')
            continue
        if sb or b in SCALAR_POINTEES or is_enum(b):
            elem = f'sizeof({b})'
            n = count_expr(a) or '1'
            const = t.startswith('const ')
            nonconst_t = t.replace('const ', '', 1) if const else t
            pre.append(f'uint32_t n{v} = {n};')
            pre.append(f'{nonconst_t} p{v} = {v} ? ({nonconst_t})wd3d_read_array({v}, n{v}, {elem}) : NULL;')
            if sb in R.FIXIN_STRUCTS:
                if not const:
                    raise Unsupported(f'output struct with pointers: {a.text}')
                pre.append(f'if (p{v}) for (uint32_t i = 0; i < n{v}; ++i) wd3d_fixin_{sb}(&p{v}[i]);')
            call_args.append(f'p{v}')
            if not const:
                post.append(f'if (p{v}) wd3d_write({v}, p{v}, n{v} * {elem});')
            continue
        raise Unsupported(f'unhandled parameter {a.text}')
    ret = p.ret.replace(' *', '*').strip()
    call = f'{p.name}({", ".join(call_args)})'
    if fail and ret == 'HRESULT':  # set_parent (void) can't fail: the wrapper is wined3d's from then on
        post = [f'if (FAILED((HRESULT)r)) {{ {" ".join(fail)} }}'] + post
    if p.name in R.PRE:
        pre.append(R.PRE[p.name])
    if p.name in R.POST:
        post.append(R.POST[p.name])
    if ret == 'void*' and (p.name.endswith('_get_parent') or p.name.endswith('_get_sub_resource_parent')):
        body = [f'uint32_t r = {R.PARENT_GUEST}({call});', *post, 'return r;']
    elif ret == 'void':
        body = [call + ';', *post, 'return 0;']
    elif ret.endswith('*'):
        body = [f'uint32_t r = (uint32_t)(uintptr_t){call};', *post, 'return r;']
    elif ret in ('float', 'double', 'UINT64', 'uint64_t', 'ULONGLONG'):
        raise Unsupported(f'return type {ret}')
    else:
        body = [f'uint32_t r = (uint32_t){call};', *post, 'return r;']
    return pre + body

def default_return(p):
    return '0x80004001u /* E_NOTIMPL */' if p.ret == 'HRESULT' else '0'

def main():
    check = '--check' in sys.argv
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    header = args[0] if args else os.path.join(REPO, 'lib/wine-wined3d/wine/include/wine/wined3d.h')
    src = open(header).read()
    protos = w.prototypes(src)
    complete = w.complete_structs(w.strip_comments(src))
    phase2 = set(l.strip() for l in open(os.path.join(HERE, R.PHASE2_IMPORTS_FILE)) if l.strip() and not l.startswith('#'))
    names = sorted(set(protos) | R.GUEST_ONLY)  # the mutex pair is __stdcall, not in the cdecl parse
    ids = {n: FIRST_ID + i for i, n in enumerate(names)}
    sig = '\n'.join(f'{n}({",".join(a.type for a in protos[n].params)})->{protos[n].ret}' if n in protos else n
                    for n in names)
    sig = R.PROTOCOL_REVISION + '\n' + sig
    proto_hash = zlib.crc32(sig.encode()) & 0xffffffff

    out = {}
    # ids
    h = ['/* Generated by tools/wined3d/gen-thunk.py from wined3d.h: do not edit. */',
         '#ifndef WD3D_THUNK_IDS_H', '#define WD3D_THUNK_IDS_H',
         f'#define WD3D_CALL_BASE 0x{CALL_BASE:08x}u', f'#define WD3D_CALL_LIMIT 0x{CALL_BASE + 0x10000:08x}u',
         f'#define WD3D_PROTOCOL_HASH 0x{proto_hash:08x}u', '#define WD3D_ID_HELLO 0',
         '#define WD3D_ID_REGISTER_WINDOW 1',
         '#define WD3D_ID_DRAIN_CALLBACKS 2', '#define WD3D_ID_DC_INFO 3', f'#define WD3D_ID_COUNT {FIRST_ID + len(names)}']
    h += [f'#define WD3D_ID_{n} {ids[n]}' for n in names]
    h += ['#endif', '']
    out['source/wined3d/thunk_ids.h'] = '\n'.join(h)

    # host dispatcher
    d = ['/* Generated by tools/wined3d/gen-thunk.py from wined3d.h: do not edit. Host side of the wined3d thunk:',
         ' * one case per export, args read from the guest stack (cdecl, 4-byte slots), structs copied as raw bytes. */',
         '#include "thunk_common.h"', '',
         'uint32_t wd3d_dispatch(uint32_t id)', '{', '    switch (id)', '    {']
    stats = {'generated': 0, 'hand': 0, 'not_yet': 0, 'later': 0}
    unsupported = []
    names_tbl = []
    for n in names:
        p = protos.get(n)
        names_tbl.append(f'    [{ids[n]}] = "{n}",')
        if n in R.GUEST_ONLY:
            continue
        if n in R.HAND:
            d += [f'    case WD3D_ID_{n}: return wd3d_hand_{n}();']
            stats['hand'] += 1
            continue
        if n not in phase2 or n in R.NOT_YET:
            d += [f'    case WD3D_ID_{n}: return wd3d_not_forwarded(id, {default_return(p)});']
            stats['not_yet' if n in phase2 else 'later'] += 1
            continue
        try:
            lines = gen_function(p, complete, R.ARRAYS)
        except Unsupported as e:
            unsupported.append(f'{n}: {e}')
            d += [f'    case WD3D_ID_{n}: return wd3d_not_forwarded(id, {default_return(p)}); /* {e} */']
            continue
        d += [f'    case WD3D_ID_{n}:', '    {'] + ['        ' + l for l in lines] + ['    }']
        stats['generated'] += 1
    d += ['    }', '    return wd3d_not_forwarded(id, 0);', '}', '',
          'const char *const wd3d_call_names[WD3D_ID_COUNT] =', '{', '    [WD3D_ID_HELLO] = "hello",',
          '    [WD3D_ID_REGISTER_WINDOW] = "register_window",', '    [WD3D_ID_DRAIN_CALLBACKS] = "drain_callbacks",',
          '    [WD3D_ID_DC_INFO] = "dc_info",'] + names_tbl + ['};', '']
    out['source/wined3d/thunk_dispatch.c'] = '\n'.join(d)

    # guest stub
    s = ['/* Generated by tools/wined3d/gen-thunk.py from wined3d.h: do not edit. The guest wined3d.dll: every export',
         ' * traps to the host with int $0x99 + a 32-bit call index (cdecl: the args stay on the guest stack), then runs',
         ' * the callbacks host wined3d queued if the host says there are any (_wd3d_pending, set by the host after every',
         ' * call). Exports that hand wined3d a window register it first (wined3d_guest.c). */',
         '    .data', '    .globl _wd3d_pending', '_wd3d_pending:', '    .long 0',
         '    .text', '    .globl _DllMain@12', '_DllMain@12:',
         '    cmpl $1, 8(%esp)          /* DLL_PROCESS_ATTACH: check the host speaks this protocol */',
         '    jne 1f', '    pushl $_wd3d_pending', f'    pushl $0x{proto_hash:08x}', '    call wd3d_hello',
         '    addl $8, %esp', '    testl %eax, %eax', '    je 2f',
         '    pushl 4(%esp)             /* hinst: d3d9/d3d8/ddraw get the guest wined3d (wined3d_guest.c) */',
         '    call _wd3d_process_attach', '    addl $4, %esp', '    movl $1, %eax', '2:  ret $12',
         '1:  movl $1, %eax', '    ret $12', 'wd3d_hello:', '    int $0x99', f'    .long 0x{CALL_BASE:08x}', '    ret',
         '    .globl _wd3d_register_window_call', '_wd3d_register_window_call:',
         '    int $0x99', f'    .long 0x{CALL_BASE + 1:08x}', '    ret',
         '    .globl _wd3d_drain_call', '_wd3d_drain_call:',
         '    int $0x99', f'    .long 0x{CALL_BASE + 2:08x}', '    ret',
         '    .globl _wd3d_dc_info_call', '_wd3d_dc_info_call:',
         '    int $0x99', f'    .long 0x{CALL_BASE + 3:08x}', '    ret',
         'wd3d_after:                   /* the host queued callbacks: run them, keeping the result */',
         '    pushl %eax', '    pushl %edx', '    call _wd3d_run_callbacks', '    popl %edx', '    popl %eax', '    ret']
    defs = ['LIBRARY wined3d.dll', 'EXPORTS']
    for n in names:
        s += [f'    .globl _{n}', f'_{n}:']
        if n in R.GUEST_ONLY:
            s += ['    ret']
        else:
            for pre in R.GUEST_PRE.get(n, []):
                if pre[0] == 'refresh':
                    s += ['    call _wd3d_refresh_windows']
                elif pre[0] == 'dc':  # the argument's address: rewritten in place
                    s += [f'    leal {4 * pre[1]}(%esp), %eax', '    pushl %eax', '    call _wd3d_dc_arg', '    addl $4, %esp']
                else:
                    fn = '_wd3d_window_arg' if pre[0] == 'window' else '_wd3d_window_desc'
                    s += [f'    movl {4 * pre[1]}(%esp), %eax', '    pushl %eax', f'    call {fn}', '    addl $4, %esp']
            s += ['    int $0x99', f'    .long 0x{CALL_BASE + ids[n]:08x}', '    cmpl $0, _wd3d_pending']
            if n in R.GUEST_POST:
                fn, arg = R.GUEST_POST[n]
                # wd3d_after keeps eax/edx and returns; then eax = fn(arg, result); arg 0 = the first argument's address
                load = f'    leal 8(%esp), %ecx' if arg == 0 else f'    movl {4 * arg + 4}(%esp), %ecx'
                s += ['    je 2f', '    call wd3d_after', '2:  pushl %eax', load, '    pushl %ecx',
                      f'    call {fn}', '    addl $8, %esp', '    ret']
            else:
                s += ['    jne wd3d_after', '    ret']
        defs.append(f'    {n}')
    defs += [f'    {n}' for n in R.GUEST_C]
    s.append('')
    defs.append('')
    out['source/wined3d/guest/wined3d_thunk.S'] = '\n'.join(s)
    out['source/wined3d/guest/wined3d.def'] = '\n'.join(defs)

    bad = False
    for rel, text in out.items():
        path = os.path.join(REPO, rel)
        if check:
            if not os.path.exists(path) or open(path).read() != text:
                print('out of date:', rel); bad = True
        else:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            open(path, 'w').write(text)
    print(f'{len(names)} exports: {stats["generated"]} generated, {stats["hand"]} hand-written, '
          f'{stats["not_yet"]} not forwarded yet (phase-2 set), {stats["later"]} for phase 3, '
          f'{len(R.GUEST_ONLY)} guest-only, {len(unsupported)} unsupported; protocol 0x{proto_hash:08x}')
    for u in unsupported:
        print('  unsupported:', u)
    sys.exit(1 if bad or unsupported else 0)

if __name__ == '__main__':
    main()
