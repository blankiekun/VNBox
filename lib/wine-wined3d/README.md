# Host wined3d sources (HOST_WINED3D=1)

Wine's wined3d and vkd3d-shader, compiled into Boxedwine as host wasm instead of being emulated in the guest. Phase
1: built and linked (`make -C project/emscripten jitJspi HOST_WINED3D=1`, output in `Build/JitJspi-wd3d`), nothing
calls it yet. The default build doesn't include any of it.

- **Version:** Wine 11.0 (`db11d0fe`, the `wine_source_commit` of `tools/wineTests/webgl-test-divergences-v47.json`),
  with that manifest's 68 production patches (`tools/d3dToWebGL/*.patch`): the same wined3d that jp.zip ships as
  `drive_c/webgl/wined3d.dll`.
- **Extra patch:** `patches/winnt-wasm.patch` keeps Wine's `winnt.h` off its x86 inline assembly when compiling for
  wasm, and declares `NtCurrentTeb` as a function (provided by `source/wined3d/sync.c`).
- **License:** LGPL 2.1 or later (`wine/COPYING.LIB`), compatible with Boxedwine's GPL.

## Files

- `sources.mk`: the files compiled and their flags (Wine's own, plus `-D__i386__ -fshort-wchar` for the Win32 32-bit
  layout and 2-byte `wchar_t`). The Vulkan backend files are left out.
- `update.sh <wine checkout>`: regenerates `wine/` (not in git, about 18 MB, 10 MB of it Wine SDK headers): a clean
  worktree at the manifest commit, the patches, the vkd3d-shader parsers (bison, flex), then every source in
  `sources.mk` and each file it includes (`emcc -MM`), plus `wined3d_exports.inc` from `wined3d.spec`. The checkout
  must be configured with its IDL headers built (`./configure`, `make -C include`).
- `files.txt`: what `update.sh` copied, for review.

## Host side

`source/wined3d/` provides what wined3d imports: msvcrt (`crt.cpp`), Wine debug channels on the Boxedwine log
(`debug.c`), kernel32/ntdll for one thread (`sync.c`; the command stream runs on the calling thread, `csmt=0`),
registry settings, modules and `GetProcAddress` (`module.c`), D3DKMT (`d3dkmt.c`), and the absent Vulkan backend
(`vulkan.c`). `exports.c` holds the table of all 328 `wined3d_*` exports and the `DllMain` entry the guest thunk
will use. user32/gdi32/wgl are placeholders (`shim_bridge_stubs.c`) until the window/GDI/GL bridge replaces them.
