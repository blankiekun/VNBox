# VNBox build targets, included by makefile (kept out of it so upstream merges stay small).

.PHONY: jitJspi

# jit plus JSPI: fd_read/fd_pread may suspend on async File reads of a lazy game folder
# (lazyfs.js jspi mode), so no SharedArrayBuffer or cross-origin isolation is needed.
# BOXEDWINE_JSPI drives the main loop through a promising export (required).
jitJspi: export BUILD_DIR := Build/JitJspi
jitJspi: export EXTRA_CPP_FLAGS := -DBOXEDWINE_WASM_JIT -DBOXEDWINE_JSPI $(WASM_JIT_SIMD_FLAG)
jitJspi: export SHELL_FILE := shell.html
jitJspi: export EXTRA_LD_FLAGS := -sJSPI --js-library=lazyfs-jspi-lib.js $(WASM_JIT_SIMD_FLAG) -sALLOW_TABLE_GROWTH $(RUNTIME_MEMORY_FLAGS) -sEXPORTED_RUNTIME_METHODS=addFunction,removeFunction,wasmTable,HEAPU32$(JIT_CCALL_EXPORTS) -sEXPORTED_FUNCTIONS=_main,_wasm_jit_import_from_file,_wasm_jit_set_persistence_active,_wasm_jit_set_record_active,_malloc,_free,_boxedwine_tick
jitJspi: export ASSET_FILES := boxedwine.css fsfs.js boxedwine-shell.js $(ZIP_ASSET_FILES)

jitJspi:
	@$(MAKE) $(BUILD_PHASE)
