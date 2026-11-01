#!/bin/sh
# build.sh <wine-src> <build-dir> <out-dir>: Wine 11.0's d3d11 and dxgi conformance tests as i386 PE exes
# (d3d11_test.exe, dxgi_test.exe), each test function wrapped by vnbox_guard.h (patch_tests.py), and the --fl9
# variants d3d11_fl9_test.exe, dxgi_fl9_test.exe (default devices down to feature level 9_1, the WebGL maximum 9_3).
# <wine-src>: a CLEAN Wine 11.0 git worktree (db11d0f; e.g. `git -C wine-src worktree add --detach wine11-clean
# db11d0f`). <build-dir> is configured once (needs i686-w64-mingw32-gcc and gcc -m32), the way winetest builds them.
set -eu
SRC=$(cd "$1" && pwd); mkdir -p "$2" "$3"; B=$(cd "$2" && pwd); OUT=$(cd "$3" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
[ "$(git -C "$SRC" rev-parse HEAD)" = db11d0fe6a169c457e23d007e20404643d067aa8 ] || { echo "build.sh: $SRC is not Wine 11.0" >&2; exit 1; }
cd "$B"
[ -f Makefile ] || "$SRC/configure" --enable-archs=i386 --without-x --without-freetype --without-wayland \
    --without-vulkan --without-gstreamer --without-pulse --without-alsa --without-cups --without-sane --without-gphoto \
    --without-krb5 --without-netapi --without-pcap --without-usb --without-v4l2 --without-sdl --without-capi \
    --without-dbus --without-opencl --without-oss --without-udev --without-unwind --without-gnutls \
    --without-fontconfig --without-inotify --without-gssapi --without-osmesa > configure.log
for v in "" fl9; do
    python3 "$HERE/patch_tests.py" "$SRC" ${v:+--$v}
    make -j"$(nproc)" dlls/d3d11/tests/i386-windows/d3d11_test.exe dlls/dxgi/tests/i386-windows/dxgi_test.exe
    for t in d3d11 dxgi; do cp dlls/$t/tests/i386-windows/${t}_test.exe "$OUT/${t}${v:+_$v}_test.exe"; done
done
ls -l "$OUT"
