#!/bin/sh
# Downloads FreeType's release tarball (version and sha256 in sources.mk), checks it and unpacks it next to this
# script as freetype-<version>/ (not in git). The tarball is kept in ~/.cache/vnbox. Called by the build.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
ver=$(sed -n 's/^FT_VERSION := //p' "$here/sources.mk")
sha=$(sed -n 's/^FT_SHA256 := //p' "$here/sources.mk")
dir="$here/freetype-$ver"
[ -f "$dir/.unpacked" ] && exit 0
cache=${XDG_CACHE_HOME:-$HOME/.cache}/vnbox/freetype-$ver.tar.xz
if ! echo "$sha  $cache" | sha256sum -c --quiet >/dev/null 2>&1; then
  mkdir -p "$(dirname "$cache")"
  for url in "https://download.savannah.gnu.org/releases/freetype/freetype-$ver.tar.xz" \
             "https://downloads.sourceforge.net/project/freetype/freetype2/$ver/freetype-$ver.tar.xz"; do
    curl -sSfL --retry 3 -o "$cache.part" "$url" && echo "$sha  $cache.part" | sha256sum -c --quiet && mv "$cache.part" "$cache" && break
  done
  echo "$sha  $cache" | sha256sum -c --quiet
fi
rm -rf "$dir" "$dir.tmp"
mkdir -p "$dir.tmp"
tar xJf "$cache" -C "$dir.tmp" --strip-components=1
touch "$dir.tmp/.unpacked"
mv "$dir.tmp" "$dir"
