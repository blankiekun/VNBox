#!/bin/sh
# update.sh <wine-src>: regenerate lib/wine-wined3d/wine/ (the host wined3d sources, HOST_WINED3D=1) from a Wine
# checkout. <wine-src> must be a git checkout of the commit in tools/wineTests/webgl-test-divergences-v47.json
# (Wine 11.0), configured with its IDL headers built (./configure, then make -C include): wined3d includes
# generated headers such as d3d11.h. This applies the manifest's production patches (the same ones jp.zip's
# wined3d.dll is built with) and patches/winnt-wasm.patch where not applied yet, generates the vkd3d-shader
# parsers (bison, flex), and copies every source in sources.mk plus each header it includes (emcc -MM).
set -eu
WINE=$(cd "$1" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
MANIFEST=$REPO/tools/wineTests/webgl-test-divergences-v47.json

want=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["wine_source_commit"])' "$MANIFEST")
have=$(git -C "$WINE" rev-parse HEAD)
[ "$have" = "$want" ] || { echo "update.sh: $WINE is at $have, the manifest wants $want" >&2; exit 1; }

# Work in a clean worktree at that commit (the patches overlap, so a patched tree can't be checked one by one),
# with configure's generated headers copied in from the configured checkout (no patch touches an .idl).
SRC=$WINE
TMP=$(mktemp -d); WINE=$TMP/wine
trap 'git -C "$SRC" worktree remove --force "$WINE" 2>/dev/null; rm -rf "$TMP"' EXIT
git -C "$SRC" worktree add -q --detach "$WINE" "$want"
python3 -c 'import json,sys; [print(p["path"]) for p in json.load(open(sys.argv[1]))["production_patches"]]' "$MANIFEST" |
    while read -r p; do git -C "$WINE" apply "$REPO/$p"; done
# The WebGL wined3d patches jp.zip's wined3d.dll is also built with (SM4 shaders: without them the GL adapter only
# reaches feature level 9_3 and d3d11 games like Muramasa reject the device).
for p in "$REPO"/vn/webgl/*.patch; do git -C "$WINE" apply "$p"; done
git -C "$WINE" apply "$HERE/patches/winnt-wasm.patch"
(cd "$SRC" && git ls-files --others --ignored --exclude-standard -- include | grep '\.h$') |
    while read -r f; do cp "$SRC/$f" "$WINE/$f"; done
[ -f "$WINE/include/d3d11.h" ] || { echo "update.sh: build Wine's IDL headers in $SRC first (make -C include)" >&2; exit 1; }

V=$WINE/libs/vkd3d/libs/vkd3d-shader
for g in hlsl preproc; do
    flex -o"$V/$g.yy.c" "$V/$g.l"
    bison -o "$V/$g.tab.c" --defines="$V/$g.tab.h" "$V/$g.y"
done

var() { printf 'p:\n\t@echo $(%s)\n' "$1" | make -s -f "$HERE/sources.mk" -f - p; }
LIST=$TMP/list
deps() { # $1 = flags variable, $2 = sources variable
    flags=$(var "$1")
    for s in $(var "$2"); do
        echo "$s" >> "$LIST"
        (cd "$WINE" && emcc $flags -MM "$s") | tr ' \\' '\n\n' | grep -v -e ':$' -e '^$' >> "$LIST"
    done
}
deps WD3D_WINED3D_FLAGS WD3D_WINED3D_SRCS
deps WD3D_VKD3D_FLAGS WD3D_VKD3D_SRCS

rm -rf "$HERE/wine"
sort -u "$LIST" | grep -v '^/' | while read -r f; do
    mkdir -p "$HERE/wine/$(dirname "$f")"
    cp "$WINE/$f" "$HERE/wine/$f"
done
# The wined3d_* exports (wined3d.spec), for source/wined3d/exports.c's table. The spec's vkd3d_* entries are
# vkd3d's D3D12 runtime, not built here.
grep -vE '^[[:space:]]*(#|$)' "$WINE/dlls/wined3d/wined3d.spec" | awk '{sub(/\(.*/, "", $3); print $3}' |
    grep '^wined3d_' | sed 's/.*/X(&)/' > "$HERE/wine/dlls/wined3d/wined3d_exports.inc"
cp "$WINE/COPYING.LIB" "$WINE/LICENSE" "$HERE/wine/" 2>/dev/null || cp "$WINE/COPYING.LIB" "$HERE/wine/"
(cd "$HERE/wine" && find . -type f | sort) > "$HERE/files.txt"
echo "update.sh: $(wc -l < "$HERE/files.txt") files, $(du -sh "$HERE/wine" | cut -f1)"
