# Host FreeType (hostfreetype.cpp): FreeType 2.11.1 compiled as host wasm, so the root fs's win32u.so can have its
# glyphs loaded and rendered there instead of by the emulated libfreetype.so.6. The sources are FreeType's release
# tarball, unpacked into lib/freetype/freetype-<version>/ (not in git) by lib/freetype/fetch.sh on the first build.
include $(PROJDIR)/lib/freetype/sources.mk
FT_DIR := $(PROJDIR)/lib/freetype/freetype-$(FT_VERSION)
FT_STAMP := $(FT_DIR)/.unpacked
FT_FLAGS_FILE := $(PROJDIR)/lib/freetype/sources.mk
FT_OBJS := $(FT_SRCS:%=$(BUILD_DIR)/freetype/%.o) $(BUILD_DIR)/freetype/sjlj.c.o
# FreeType's own setjmp/longjmp (sjlj.c) needs wasm exceptions, as the JIT builds have anyway.
FT_CC := $(CC) $(OPTIMIZATION_FLAGS) $(TAIL_CALL_FLAG) $(WASM_EXCEPTIONS_FLAG) -sSUPPORT_LONGJMP=wasm -sUSE_ZLIB=1
ALL += $(FT_OBJS)
CPPFLAGS += -DBOXEDWINE_HOST_FREETYPE -I$(FT_DIR)/include
-include $(FT_OBJS:.o=.d)

$(FT_STAMP): $(PROJDIR)/lib/freetype/fetch.sh $(FT_FLAGS_FILE)
	@sh $(PROJDIR)/lib/freetype/fetch.sh
$(FT_SRCS:%=$(FT_DIR)/%) $(FT_DIR)/include/ft2build.h: | $(FT_STAMP) ;
# Host code that includes FreeType's headers needs them unpacked first.
$(BUILD_DIR)/$(PROJDIR)/source/emulation/cpu/normal/hostfreetype.cpp.o $(BUILD_DIR)/$(PROJDIR)/source/test/cpu/testHostFreetype.cpp.o: | $(FT_STAMP)

$(BUILD_DIR)/freetype/%.c.o: $(FT_DIR)/%.c $(COMPILE_CONFIG) $(FT_FLAGS_FILE)
	@$(MKDIR_P) $(dir $@)
	@$(FT_CC) $(patsubst -I%,-I$(FT_DIR)/%,$(FT_FLAGS)) -MMD -MP -c $< -o $@
$(BUILD_DIR)/freetype/sjlj.c.o: $(PROJDIR)/lib/freetype/sjlj.c $(COMPILE_CONFIG)
	@$(MKDIR_P) $(dir $@)
	@$(FT_CC) -MMD -MP -c $< -o $@

# testHostFreetype.cpp's font (a hinted TrueType font already in the tree).
ifneq (,$(findstring -D__TEST,$(EXTRA_CPP_FLAGS)))
LDFLAGS += --embed-file $(PROJDIR)/lib/imgui/misc/fonts/Cousine-Regular.ttf@/hostft/Cousine-Regular.ttf
endif
