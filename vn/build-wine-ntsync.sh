#!/usr/bin/env bash
# Builds the root fs's Wine 11.0 ntdll.so and wineserver again with Linux's ntsync.h, for jp.zip.
# Wine 11 serves event, mutex and semaphore waits in-process only through /dev/ntsync, and compiles
# that code only when linux/ntsync.h exists at configure time (dlls/ntdll/unix/sync.c,
# server/inproc_sync.c; no other file uses it). The root fs's Wine was built on Ubuntu 24.04, whose
# kernel headers (6.8) lack it, so it never uses ntsync. With the header, both still fall back to
# wineserver waits when /dev/ntsync can't be opened, as now.
# Same source, patches (tools/buildWine/wine_builds.json's 11.0 patches), compiler and flags as the
# root fs. With NTSYNC=0 (no header), both files are byte-identical to the root fs's (v0.1.3).
# Wine is LGPL-2.1: build-jp-overlay.sh ships the source notes. Needs Docker.
# Usage: vn/build-wine-ntsync.sh <out dir>  (writes ntdll.so, wineserver and patches/, the patches applied)
set -euo pipefail
out=$(realpath -m "$1")
here=$(cd "$(dirname "$0")" && pwd)
url=https://dl.winehq.org/wine/source/11.0/wine-11.0.tar.xz
sha=c07a6857933c1fc60dff5448d79f39c92481c1e9db5aa628db9d0358446e0701
# The ntsync uapi is unchanged from 6.14 (first release with the full driver) to 6.17.
hdr_url=https://raw.githubusercontent.com/torvalds/linux/v6.14/include/uapi/linux/ntsync.h
hdr_sha=006437ee52a3e04f921df77081eb5c21c44c71f598b10ac534c6ef9e78296262
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

curl -sSfL -o "$work/wine.tar.xz" "$url"
echo "$sha  $work/wine.tar.xz" | sha256sum -c --quiet
if [ "${NTSYNC:-1}" != 0 ]; then
  curl -sSfL -o "$work/ntsync.h" "$hdr_url"
  echo "$hdr_sha  $work/ntsync.h" | sha256sum -c --quiet
fi
mkdir -p "$work/patches"
python3 -I - "$here/../tools/buildWine" "$work/patches" <<'PY'
import json, shutil, sys
src, dst = sys.argv[1:]
ver = lambda s: tuple(int(x) for x in s.split("."))
ops = json.load(open(f"{src}/wine_builds.json"))["operations"]
n = 0
for op in ops:
    w = op.get("when", {})
    if not ver(w.get("min", "0")) <= (11, 0) <= ver(w.get("max", "999")):
        continue
    assert set(op) <= {"when", "reason", "apply_patch"}, f"unsupported 11.0 operation: {op}"
    n += 1
    shutil.copy(f"{src}/patches/{op['apply_patch']}", f"{dst}/{n:03d}-{op['apply_patch']}")
PY
mkdir -p "$out"
docker run --rm -v "$work:/w" -v "$out:/out" -e HOST_UID="$(id -u)" ubuntu:24.04@sha256:24737cef17f4e22b1977df7d9a05e77c4e28b7da5b8911f8d8ccda136bf3573c bash -euo pipefail -c '
  # libc6-dev:i386 as in build_wine.py: wineserver links its libc_nonshared.a (CET-built atexit).
  dpkg --add-architecture i386 && apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    --no-install-recommends gcc-multilib libc6-dev:i386 flex bison make xz-utils patch >/dev/null
  gcc -v 2>&1 | grep -q "13.3.0-6ubuntu2~24.04.1" || echo "warning: GCC differs from the root fs build (13.3.0-6ubuntu2~24.04.1)" >&2
  [ ! -f /w/ntsync.h ] || install -m 644 /w/ntsync.h /usr/include/linux/ntsync.h
  cd /tmp && tar xf /w/wine.tar.xz && cd wine-11.0
  for p in /w/patches/*.patch; do patch -p1 -s --no-backup-if-mismatch < "$p"; done
  CC="gcc -m32" CXX="g++ -m32" CFLAGS="-DNDEBUG -O2 -msse2 -march=pentium4 -mfpmath=sse" \
    LD="ld -m elf_i386" TARGETFLAGS="-m32" LDFLAGS="-s" \
    ./configure --without-mingw --disable-tests --prefix=/opt/wine --without-x --without-freetype \
    --without-gnutls --without-gstreamer --without-sdl --without-fontconfig --without-opengl \
    --without-vulkan --without-oss --without-alsa --without-pulse --without-cups --without-dbus >/dev/null
  [ ! -f /w/ntsync.h ] || grep -q "define HAVE_LINUX_NTSYNC_H 1" include/config.h
  make -j"${JOBS:-4}" dlls/ntdll/ntdll.so server/wineserver >/tmp/make.log 2>&1 || { tail -40 /tmp/make.log; exit 1; }
  install -o "$HOST_UID" -g "$HOST_UID" -m 755 dlls/ntdll/ntdll.so server/wineserver /out/
'
rm -rf "$out/patches"
cp -r "$work/patches" "$out/patches"
