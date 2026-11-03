# Host DIB primitives (hostdibdrv.cpp): Wine 11.0's dlls/win32u/dibdrv/primitives.c (lib/wine-dibdrv, LGPL-2.1+)
# compiled as host wasm, so the patched win32u.so's per-pixel drawing (glyph blending, fills, blits) runs there
# instead of as emulated x86. -DNDEBUG as Wine's own build: its asserts don't run in the guest either.
DIBDRV_DIR := $(PROJDIR)/lib/wine-dibdrv
DIBDRV_OBJS := $(BUILD_DIR)/wine-dibdrv/primitives.c.o $(BUILD_DIR)/wine-dibdrv/host.c.o
ALL += $(DIBDRV_OBJS)
CPPFLAGS += -DBOXEDWINE_HOST_DIBDRV
-include $(DIBDRV_OBJS:.o=.d)

$(BUILD_DIR)/wine-dibdrv/%.c.o: $(DIBDRV_DIR)/%.c $(COMPILE_CONFIG)
	@$(MKDIR_P) $(dir $@)
	@$(CC) $(OPTIMIZATION_FLAGS) $(TAIL_CALL_FLAG) $(filter -pthread,$(EXTRA_CPP_FLAGS)) -DNDEBUG -I$(DIBDRV_DIR) -MMD -MP -c $< -o $@
