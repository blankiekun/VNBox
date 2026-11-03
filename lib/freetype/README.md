# Host FreeType

FreeType compiled into Boxedwine as host wasm, for `source/emulation/cpu/normal/hostfreetype.cpp`: the FreeType calls
the root fs's `win32u.so` makes on the fonts it draws with run there instead of in the emulated
`/usr/local/lib/libfreetype.so.6`. `nativeFreetype=0` (URL) turns that off.

- **Version:** 2.11.1, the root fs's library (`libfreetype.so.6.18.1`, `FT_Library_Version` 2.11.1). Not the Emscripten
  port (2.13.3): its hinting and rasterizer give other bytes.
- **Source:** FreeType's release tarball (`sources.mk`: version, sha256, files, flags), downloaded and unpacked by
  `fetch.sh` into `freetype-<version>/` (not in git) on the first build; the tarball is kept in `~/.cache/vnbox`.
- **Options:** as the root fs build: ClearType-style LCD filtering (`FT_CONFIG_OPTION_SUBPIXEL_RENDERING`), zlib, the
  default modules. No PNG (only for color bitmap glyphs). The root fs library is 32-bit x86 built without SSE2, so its
  smooth rasterizer splits Bézier arcs (no DDA); wasm takes the same path.
- **setjmp/longjmp:** FreeType longjmps out of a rasterizer overflow and a broken cmap. Its files are compiled with
  `-sSUPPORT_LONGJMP=wasm`; `sjlj.c` adds the two runtime pieces the rest of the build (`SUPPORT_LONGJMP=0`) lacks.
- **License:** FreeType License or GPLv2 (`freetype-<version>/LICENSE.TXT`), compatible with Boxedwine's GPL.

## Same bytes as the root fs library

`ftcompare.c` hashes what win32u gets from FreeType for every glyph of a font at 9 sizes and 5 load flags (GGO's).
Checked (2026-11): the root fs `libfreetype.so.6` (run natively, 32-bit, with the root fs's libz and libpng16), a
native `gcc -m32 -mno-sse -mno-sse2` build of these sources and flags, and the wasm objects of this build (under node)
print identical lines for jp.zip's IPA fonts (ipag, ipagp, ipam, ipamp: all 12728 glyphs each) and the root fs's
Liberation and Wine fonts (.ttf and .fon). Without `FT_CONFIG_OPTION_SUBPIXEL_RENDERING` LCD glyphs differ; with SSE2
some gray pixels differ by one.

    gcc -m32 -Ifreetype-2.11.1/include ftcompare.c -o ftcompare -ldl
    LD_LIBRARY_PATH=<dir with the root fs libfreetype.so.6, libz.so.1, libpng16.so.16> ./ftcompare <that dir>/libfreetype.so.6 fonts... > a.txt
    ./ftcompare <other build>.so fonts... > b.txt; cmp a.txt b.txt
