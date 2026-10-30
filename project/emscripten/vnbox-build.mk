# VNBox build rules, included by makefile inside a target's build (BUILD_DIR set).

# The page's movie decoder (vnbox-movie.js): pl_mpeg (lib/pl_mpeg, MIT), wasm inlined in the .js.
MOVIE_DECODER := $(BUILD_DIR)/vnbox-movie-decoder.js

copy-assets: $(MOVIE_DECODER)

$(MOVIE_DECODER): vnbox-movie-decoder.c $(PROJDIR)/lib/pl_mpeg/pl_mpeg.h
	@$(MKDIR_P) $(dir $@)
	@$(CC) -O3 -msimd128 $< -o $@.tmp.js -sMODULARIZE -sEXPORT_NAME=VnboxMovieDecoder -sSINGLE_FILE -sENVIRONMENT=web \
		-sALLOW_MEMORY_GROWTH -sFILESYSTEM=0 -sEXPORTED_FUNCTIONS=_malloc,_free -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPF32
	@{ echo "/*!"; cat $(PROJDIR)/lib/pl_mpeg/LICENSE; echo "*/"; cat $@.tmp.js; } > $@ && rm $@.tmp.js
