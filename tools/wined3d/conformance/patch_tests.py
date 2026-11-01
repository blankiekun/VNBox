#!/usr/bin/env python3
"""patch_tests.py <wine-tree> [--fl9]: puts vnbox_guard.h around every test function of Wine 11.0's d3d11 and dxgi tests
(dlls/d3d11/tests/d3d11.c, dlls/dxgi/tests/dxgi.c) so one run in the emulator reports each function's counts and
survives crashes. Test bodies are not changed. Idempotent (works from the git version of each file).
--fl9: the default devices also take feature levels 9_3..9_1 (d3d11's create_device: 11_0..10_0; dxgi's
create_d3d11_device: 11_0 only). The WebGL wined3d tops out at 9_3, so without it nearly every test skips; with it
many fail on any WebGL build (SM4 shaders at 9_x), which still compares guest wined3d against host wined3d."""
import pathlib, re, shutil, subprocess, sys

tree = pathlib.Path(sys.argv[1])
fl9 = '--fl9' in sys.argv[2:]
here = pathlib.Path(__file__).resolve().parent


def git_original(rel):
    return subprocess.run(['git', '-C', str(tree), 'show', 'HEAD:' + rel], check=True, capture_output=True,
                          text=True).stdout


def sub1(pattern, repl, s, flags=0):
    out, n = re.subn(pattern, repl, s, count=1, flags=flags)
    if n != 1:
        sys.exit(f'patch_tests.py: pattern not found: {pattern}')
    return out


QUEUE_MACROS = '''
/* VNBox: name each queued test (vnbox_guard.h) */
#define queue_test(f) (vnbox_qname = #f, queue_test(f))
#define queue_for_each_feature_level(f) (vnbox_qname = #f, queue_for_each_feature_level(f))
#define queue_for_each_9_x_feature_level(f) (vnbox_qname = #f, queue_for_each_9_x_feature_level(f))
#define queue_for_each_feature_level_in_range(a, b, f) (vnbox_qname = #f, queue_for_each_feature_level_in_range(a, b, f))

'''


def wrap_direct_calls(s):
    """Calls after run_queued_tests() in START_TEST run directly: wrap them too."""
    start = s.index('START_TEST(')
    head, body = s[:start], s[start:]
    body = sub1(r'(START_TEST\(\w+\)\n\{\n(?:    [^\n]*;\n)*)', r'\1\n    vnbox_init();\n', body)
    body = re.sub(r'^    (test_\w+)\(\);$', r'    VNBOX_CALL("\1", 0, \1());', body, flags=re.M)
    body = re.sub(r'^    (run_on_d3d1[02])\((\w+)\);$', r'    VNBOX_CALL("\2:\1", 0, \1(\2));', body, flags=re.M)
    return head + QUEUE_MACROS + body


def patch_d3d11(s):
    if fl9:
        s = sub1(r'(default_feature_level\[\] =\n    \{\n(?:        D3D_FEATURE_LEVEL_1[01]_[01],\n)+)',
                 r'\1        D3D_FEATURE_LEVEL_9_3,\n        D3D_FEATURE_LEVEL_9_2,\n        D3D_FEATURE_LEVEL_9_1,\n', s)
    s = sub1(r'\nstatic void queue_test_entry\(', '\n#include "vnbox_guard.h"\n\nstatic void queue_test_entry(', s)
    s = sub1(r'    mt_tests\[mt_test_count\+\+\] = \*t;',
             '    vnbox_names[mt_test_count] = vnbox_qname;\n    mt_tests[mt_test_count++] = *t;', s)
    s = sub1(r'            run_mt_test\(&mt_tests\[i\]\);',
             '            VNBOX_CALL(vnbox_names[i], mt_tests[i].fl, run_mt_test(&mt_tests[i]));', s)
    return wrap_direct_calls(s)


def patch_dxgi(s):
    if fl9:
        s = sub1(r'(create_d3d11_device\(void\)\n\{\n    static const D3D_FEATURE_LEVEL feature_level\[\] =\n    \{\n        D3D_FEATURE_LEVEL_11_0,\n)',
                 r'\1        D3D_FEATURE_LEVEL_10_1,\n        D3D_FEATURE_LEVEL_10_0,\n        D3D_FEATURE_LEVEL_9_3,\n'
                 r'        D3D_FEATURE_LEVEL_9_2,\n        D3D_FEATURE_LEVEL_9_1,\n', s)
    s = sub1(r'\nstatic void queue_test\(', '\n#include "vnbox_guard.h"\n\nstatic void queue_test(', s)
    s = sub1(r'    mt_tests\[mt_test_count\+\+\]\.test = test;',
             '    vnbox_names[mt_test_count] = vnbox_qname;\n    mt_tests[mt_test_count++].test = test;', s)
    s = sub1(r'            mt_tests\[i\]\.test\(\);', '            VNBOX_CALL(vnbox_names[i], 0, mt_tests[i].test());', s)
    return wrap_direct_calls(s)


# wine/test.h: "### EXIT" after the summary line of the main process (not d3d11's test_create_device_child)
TEST_H = 'include/wine/test.h'
(tree / TEST_H).write_text(sub1(r'(\n    status = winetest_failures \+ winetest_todo_failures;)',
    r'\n    if (winetest_argc != 3 || !strncmp(winetest_argv[2], "--", 2)) printf("### EXIT\\n");\1', git_original(TEST_H)))

for rel, fn in (('dlls/d3d11/tests/d3d11.c', patch_d3d11), ('dlls/dxgi/tests/dxgi.c', patch_dxgi)):
    (tree / rel).write_text(fn(git_original(rel)))
    shutil.copy(here / 'vnbox_guard.h', tree / pathlib.Path(rel).parent / 'vnbox_guard.h')
    print('patched', rel)
