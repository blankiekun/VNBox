#!/usr/bin/env bash
# Builds the root fs's WebGL WineD3D (C:\webgl\wined3d.dll, a PE32 DLL) again with vn/webgl/*.patch,
# for jp.zip: Wine 11.0 with the v47 WebGL series (tools/wineTests/webgl-test-divergences-v47.json,
# each patch's hash checked), configured and built as tools/d3dToWebGL's build-boxedwine-webgl-dlls.sh
# does, wined3d only. With WEBGL_FIX=0 (no vn/webgl patches) the DLL is byte-identical to the root
# fs's (tools/buildWine/webgl_filesystems_v23.json); the script checks that hash then.
# Wine is LGPL-2.1: build-jp-overlay.sh ships the patches. Needs Docker.
# Usage: vn/build-webgl-wined3d.sh <out dir>  (writes wined3d.dll)
set -euo pipefail
out=$(realpath -m "$1")
here=$(cd "$(dirname "$0")" && pwd)
url=https://dl.winehq.org/wine/source/11.0/wine-11.0.tar.xz
sha=c07a6857933c1fc60dff5448d79f39c92481c1e9db5aa628db9d0358446e0701
upstream_sha=1b624780d0e274c59706f120f1e190cc11daa5848da9f4b6a1558c40f939bbec
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

curl -sSfL --retry 5 --retry-all-errors -o "$work/wine.tar.xz" "$url"
echo "$sha  $work/wine.tar.xz" | sha256sum -c --quiet
# The series in order as 000-name.patch, then VNBox's.
mkdir -p "$work/patches"
python3 -I - "$here/.." "$work/patches" <<'PY'
import hashlib, json, os, shutil, sys
repo, dest = sys.argv[1:]
series = json.load(open(os.path.join(repo, "tools/wineTests/webgl-test-divergences-v47.json")))["production_patches"]
for i, p in enumerate(series):
    src = os.path.join(repo, p["path"])
    if hashlib.sha256(open(src, "rb").read()).hexdigest() != p["sha256"]:
        sys.exit("hash mismatch: " + p["path"])
    shutil.copy(src, os.path.join(dest, "%03d-%s" % (i, os.path.basename(src))))
PY
if [ "${WEBGL_FIX:-1}" != 0 ]; then
  for p in "$here"/webgl/*.patch; do cp "$p" "$work/patches/900-$(basename "$p")"; done
fi
mkdir -p "$out"
docker run --rm -v "$work:/w" -v "$out:/out" -e HOST_UID="$(id -u)" -e JOBS="${JOBS:-$(nproc)}" ubuntu:24.04@sha256:24737cef17f4e22b1977df7d9a05e77c4e28b7da5b8911f8d8ccda136bf3573c bash -euo pipefail -c '
  apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
    gcc-multilib gcc-mingw-w64-i686 binutils-mingw-w64-i686 flex bison make xz-utils patch >/dev/null
  # Under /usr/src/wine, the path -ffile-prefix-map gives the upstream build.
  mkdir -p /usr/src && cd /usr/src && tar xf /w/wine.tar.xz && mv wine-11.0 wine && cd wine
  for p in /w/patches/*.patch; do patch -p1 -s --no-backup-if-mismatch < "$p"; done
  export SOURCE_DATE_EPOCH=1768319767
  mkdir -p /b/tools /b/win32
  (cd /b/tools && /usr/src/wine/configure --without-x --without-freetype >/dev/null)
  make -C /b/tools -j"$JOBS" __tooldeps__ >/tmp/make.log 2>&1 || { tail -40 /tmp/make.log; exit 1; }
  (cd /b/win32 && /usr/src/wine/configure --without-x --disable-kernel32 --without-freetype --host=i686-w64-mingw32 \
    CFLAGS="-O3 -msse2 -march=pentium4 -mfpmath=sse -DWINE_NO_DEBUG_MSGS -DWINE_NOWINSOCK -DUSE_WIN32_OPENGL -DUSE_WIN32_VULKAN -ffile-prefix-map=/usr/src/wine=/usr/src/wine" \
    --with-wine-tools=/b/tools/ LDFLAGS=" -static-libgcc -Wl,--no-insert-timestamp" >/dev/null)
  make -C /b/win32 -j"$JOBS" dlls/wined3d/wined3d.dll >/tmp/make.log 2>&1 || { tail -40 /tmp/make.log; exit 1; }
  install -o "$HOST_UID" -g "$HOST_UID" -m 644 /b/win32/dlls/wined3d/wined3d.dll /out/
'
if [ "${WEBGL_FIX:-1}" = 0 ]; then
  echo "$upstream_sha  $out/wined3d.dll" | sha256sum -c --quiet || { echo "wined3d.dll differs from the root fs's" >&2; exit 1; }
fi
