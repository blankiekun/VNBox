#!/usr/bin/env bash
# Builds VNBox's patched Wine 11.0 wineboot and explorer (vn/wine/*.patch) for jp.zip: the
# root fs's Wine is built --without-mingw, so the code is in i386-unix/<name>.so (ELF, gcc -m32)
# and only those two files change. Same compiler as the root fs (Ubuntu 24.04's GCC 13.3) and the
# same flags as tools/buildWine/wine_builds.json; no other Wine 11.0 patch touches these programs.
# Wine is LGPL-2.1: jp.zip also carries the patches (opt/wine/share/wine/vnbox-patches/).
# Built without the patches, both files are byte-identical to the root fs's (v0.1.3); translated
# resources need gettext's msgfmt. Needs Docker.
# Usage: vn/build-wine-boot.sh <out dir>  (writes wineboot.exe.so, explorer.exe.so)
set -euo pipefail
out=$(realpath -m "$1")
here=$(cd "$(dirname "$0")" && pwd)
url=https://dl.winehq.org/wine/source/11.0/wine-11.0.tar.xz
sha=c07a6857933c1fc60dff5448d79f39c92481c1e9db5aa628db9d0358446e0701
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

curl -sSfL -o "$work/wine.tar.xz" "$url"
echo "$sha  $work/wine.tar.xz" | sha256sum -c --quiet
cp "$here"/wine/*.patch "$work/"
mkdir -p "$out"
docker run --rm -v "$work:/w" -v "$out:/out" -e HOST_UID="$(id -u)" ubuntu:24.04@sha256:24737cef17f4e22b1977df7d9a05e77c4e28b7da5b8911f8d8ccda136bf3573c bash -euo pipefail -c '
  apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
    gcc-multilib flex bison gettext make xz-utils patch >/dev/null
  gcc -v 2>&1 | grep -q "13.3.0-6ubuntu2~24.04.1" || echo "warning: GCC differs from the root fs build (13.3.0-6ubuntu2~24.04.1)" >&2
  cd /tmp && tar xf /w/wine.tar.xz && cd wine-11.0
  for p in /w/*.patch; do patch -p1 --no-backup-if-mismatch < "$p"; done
  CC="gcc -m32" CXX="g++ -m32" CFLAGS="-DNDEBUG -O2 -msse2 -march=pentium4 -mfpmath=sse" \
    LD="ld -m elf_i386" TARGETFLAGS="-m32" LDFLAGS="-s" \
    ./configure --without-mingw --disable-tests --prefix=/opt/wine --without-x --without-freetype \
    --without-gnutls --without-gstreamer --without-sdl --without-fontconfig --without-opengl \
    --without-vulkan --without-oss --without-alsa --without-pulse --without-cups --without-dbus >/dev/null
  make -j"${JOBS:-4}" programs/wineboot/wineboot.exe.so programs/explorer/explorer.exe.so \
    >/tmp/make.log 2>&1 || { tail -40 /tmp/make.log; exit 1; }
  install -o "$HOST_UID" -g "$HOST_UID" -m 755 programs/wineboot/wineboot.exe.so programs/explorer/explorer.exe.so /out/
'
