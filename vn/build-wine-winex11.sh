#!/usr/bin/env bash
# Builds the root fs's Wine 11.0 winex11.so again with vn/wine/winex11-*.patch, for jp.zip (window
# surface flushes skip setting the alpha byte of every pixel, which VNBox's X server never reads).
# Same source, root fs patches (tools/buildWine/wine_builds.json's 11.0 patches), compiler, flags
# and configure options as the root fs (but OSS, and no Vulkan as in the web root fs), with the X, GL and EGL headers it was built against.
# With WINEX11_PATCHES=0, winex11.so is byte-identical to the root fs's (v0.2.8, checked).
# Wine is LGPL-2.1: build-jp-overlay.sh ships the patches. Needs Docker.
# Usage: vn/build-wine-winex11.sh <out dir>  (writes winex11.so)
set -euo pipefail
out=$(realpath -m "$1")
here=$(cd "$(dirname "$0")" && pwd)
url=https://dl.winehq.org/wine/source/11.0/wine-11.0.tar.xz
sha=c07a6857933c1fc60dff5448d79f39c92481c1e9db5aa628db9d0358446e0701
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# One download for all wine builds: dl.winehq.org answers a second fetch within a minute with 429.
cache=${XDG_CACHE_HOME:-$HOME/.cache}/vnbox/wine-11.0.tar.xz
if ! echo "$sha  $cache" | sha256sum -c --quiet 2>/dev/null; then
  mkdir -p "$(dirname "$cache")"
  curl -sSfL --retry 5 -o "$cache" "$url"
  echo "$sha  $cache" | sha256sum -c --quiet
fi
cp "$cache" "$work/wine.tar.xz"
mkdir -p "$work/patches"
python3 -I - "$here/../tools/buildWine" "$work/patches" "$work/configure-args" <<'PY'
import json, shutil, sys
src, dst, args = sys.argv[1:]
ver = lambda s: tuple(int(x) for x in s.split("."))
builds = json.load(open(f"{src}/wine_builds.json"))
n = 0
for op in builds["operations"]:
    w = op.get("when", {})
    if not ver(w.get("min", "0")) <= (11, 0) <= ver(w.get("max", "999")):
        continue
    assert set(op) <= {"when", "reason", "apply_patch"}, f"unsupported 11.0 operation: {op}"
    n += 1
    shutil.copy(f"{src}/patches/{op['apply_patch']}", f"{dst}/{n:03d}-{op['apply_patch']}")
# --with-oss needs the root fs build's soundcard.h (OSS is not in winex11); the web root fs has no Vulkan.
args_ = [a for a in builds["build"]["configure_args"] if a not in ("--with-oss", "--with-vulkan")]
open(args, "w").write(" ".join(args_ + ["--without-vulkan"]))
PY
if [ "${WINEX11_PATCHES:-1}" != 0 ]; then
  for p in "$here"/wine/winex11-*.patch; do cp "$p" "$work/patches/900-$(basename "$p")"; done
fi
mkdir -p "$out"
docker run --rm --network host -v "$work:/w" -v "$out:/out" -e HOST_UID="$(id -u)" ubuntu:24.04@sha256:24737cef17f4e22b1977df7d9a05e77c4e28b7da5b8911f8d8ccda136bf3573c bash -euo pipefail -c '
  dpkg --add-architecture i386 && apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    --no-install-recommends gcc-multilib libc6-dev:i386 flex bison make xz-utils patch pkg-config \
    libx11-dev:i386 libxext-dev:i386 libxrender-dev:i386 libxrandr-dev:i386 libxi-dev:i386 libxfixes-dev:i386 \
    libxcursor-dev:i386 libxinerama-dev:i386 libxxf86vm-dev:i386 libgl-dev:i386 libegl-dev:i386 \
    libfreetype-dev:i386 libfontconfig-dev:i386 >/dev/null
  gcc -v 2>&1 | grep -q "13.3.0-6ubuntu2~24.04.1" || echo "warning: GCC differs from the root fs build (13.3.0-6ubuntu2~24.04.1)" >&2
  cd /tmp && tar xf /w/wine.tar.xz && cd wine-11.0
  for p in /w/patches/*.patch; do patch -p1 -s --no-backup-if-mismatch < "$p"; done
  PKG_CONFIG_LIBDIR=/usr/lib/i386-linux-gnu/pkgconfig:/usr/lib/pkgconfig:/usr/share/pkgconfig \
  CC="gcc -m32" CXX="g++ -m32" CFLAGS="-DNDEBUG -O2 -msse2 -march=pentium4 -mfpmath=sse" \
    LD="ld -m elf_i386" TARGETFLAGS="-m32" LDFLAGS="-s" \
    ./configure $(cat /w/configure-args) >/dev/null
  make -j"${JOBS:-4}" dlls/winex11.drv/winex11.so >/tmp/make.log 2>&1 || { tail -40 /tmp/make.log; exit 1; }
  install -o "$HOST_UID" -g "$HOST_UID" -m 755 dlls/winex11.drv/winex11.so /out/
'
