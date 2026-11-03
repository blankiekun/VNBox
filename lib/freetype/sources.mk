# Host FreeType (source/emulation/cpu/normal/hostfreetype.cpp): the version the root fs's /usr/local/lib/libfreetype.so.6
# is (2.11.1, libtool 6.18.1), built with the options that change its output the same way, so a glyph comes out
# byte-identical. Read by project/emscripten/vnbox-freetype.mk and fetch.sh (keep each list on one line).
FT_VERSION := 2.11.1
FT_SHA256 := 3333ae7cfda88429c97a7ae63b7d01ab398076c3b67182e960e5684050f2c5c8
# FreeType's default module set (include/freetype/config/ftmodule.h), no validators (gxvalid, otvalid), no bzip2: as the root fs.
FT_SRCS := src/base/ftsystem.c src/base/ftinit.c src/base/ftdebug.c src/base/ftbase.c src/base/ftbbox.c src/base/ftbdf.c src/base/ftbitmap.c src/base/ftcid.c src/base/ftfstype.c src/base/ftgasp.c src/base/ftglyph.c src/base/ftgxval.c src/base/ftmm.c src/base/ftotval.c src/base/ftpatent.c src/base/ftpfr.c src/base/ftstroke.c src/base/ftsynth.c src/base/fttype1.c src/base/ftwinfnt.c src/autofit/autofit.c src/bdf/bdf.c src/cache/ftcache.c src/cff/cff.c src/cid/type1cid.c src/gzip/ftgzip.c src/lzw/ftlzw.c src/pcf/pcf.c src/pfr/pfr.c src/psaux/psaux.c src/pshinter/pshinter.c src/psnames/psnames.c src/raster/raster.c src/sdf/sdf.c src/sfnt/sfnt.c src/smooth/smooth.c src/truetype/truetype.c src/type1/type1.c src/type42/type42.c src/winfonts/winfnt.c
# The root fs build has ClearType-style LCD filtering (FT_Library_SetLcdFilter works) and zlib. Not PNG (color bitmap
# glyphs, which no font VNBox ships has). Its rasterizer takes the non-SSE2 path (32-bit x86 without -msse2), as wasm does.
# FreeType longjmps out of a rasterizer overflow and a broken cmap: wasm setjmp/longjmp for these files (sjlj.c).
FT_FLAGS := -DFT2_BUILD_LIBRARY -DFT_CONFIG_OPTION_SYSTEM_ZLIB -DFT_CONFIG_OPTION_SUBPIXEL_RENDERING -Iinclude -w
