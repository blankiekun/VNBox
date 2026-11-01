# Host wined3d (HOST_WINED3D=1, default off): Wine's wined3d + vkd3d-shader compiled as host wasm and linked in,
# with the Win32 functions they import provided by source/wined3d/. Phase 1: linked but unused.
# The Wine sources live in lib/wine-wined3d/wine/ (not in git: lib/wine-wined3d/update.sh prepares them).
ifeq ($(HOST_WINED3D),1)

WD3D_DIR := $(PROJDIR)/lib/wine-wined3d/wine
include $(PROJDIR)/lib/wine-wined3d/sources.mk
ifeq ($(wildcard $(WD3D_DIR)/dlls/wined3d/device.c),)
$(error HOST_WINED3D=1: $(WD3D_DIR) is missing; run lib/wine-wined3d/update.sh <wine checkout> first)
endif

# The flags live in sources.mk: a change there rebuilds every wined3d object.
WD3D_FLAGS_FILE := $(PROJDIR)/lib/wine-wined3d/sources.mk
wd3d_abs = $(patsubst -I%,-I$(WD3D_DIR)/%,$(1))
WD3D_CC := $(CC) $(OPTIMIZATION_FLAGS) $(TAIL_CALL_FLAG)
WD3D_OBJS := $(WD3D_WINED3D_SRCS:%=$(BUILD_DIR)/wd3d/%.o) $(WD3D_VKD3D_SRCS:%=$(BUILD_DIR)/wd3d/%.o)
# The C shims are compiled against Wine's headers (exact Win32 prototypes); the .cpp ones are host code.
WD3D_SHIM_C := $(wildcard $(PROJDIR)/source/wined3d/*.c)
WD3D_SHIM_OBJS := $(WD3D_SHIM_C:$(PROJDIR)/source/wined3d/%=$(BUILD_DIR)/wd3d/shim/%.o)
WD3D_HOST_OBJS := $(patsubst %,$(BUILD_DIR)/%.o,$(wildcard $(PROJDIR)/source/wined3d/*.cpp))

$(WD3D_HOST_OBJS): INCLUDES := $(COMMON_INCLUDES)
ALL += $(WD3D_OBJS) $(WD3D_SHIM_OBJS) $(WD3D_HOST_OBJS)
CPPFLAGS += -DBOXEDWINE_HOST_WINED3D
# Keeps the export table, and through it all of wined3d, in the link (source/wined3d/exports.c).
LDFLAGS += -Wl,--export=wd3d_link_root -Wl,--export=wd3d_dll_main
# Static data from 64 KiB (Emscripten's default: 1 KiB): a NULL + offset write in host wined3d (no trap in wasm, an
# access violation in the guest wined3d) hits unused memory instead of the emulator's globals (thunkHost.cpp).
LDFLAGS += -sGLOBAL_BASE=65536
# The guest side: the stub wined3d.dll (PE32, MinGW), embedded at /hostwd3d/wined3d.dll; startupArgs.cpp puts it in
# system32 in place of the guest's wined3d.
WD3D_GUEST_DIR := $(PROJDIR)/source/wined3d/guest
WD3D_STUB := $(BUILD_DIR)/wd3d/guest/wined3d.dll
MINGW_CC ?= i686-w64-mingw32-gcc
$(WD3D_STUB): $(WD3D_GUEST_DIR)/wined3d_thunk.S $(WD3D_GUEST_DIR)/wined3d_guest.c $(WD3D_GUEST_DIR)/wined3d.def
	@$(MKDIR_P) $(dir $@)
	@$(MINGW_CC) -shared -nostdlib -O2 -ffreestanding -fno-asynchronous-unwind-tables -fno-stack-protector -Wall $^ \
		-Wl,--kill-at -e _DllMain@12 -luser32 -lgdi32 -lkernel32 -o $@
$(BUILD_DIR)/$(TARGET_EXEC): $(WD3D_STUB)
LDFLAGS += --embed-file $(WD3D_STUB)@/hostwd3d/wined3d.dll
-include $(WD3D_OBJS:.o=.d) $(WD3D_SHIM_OBJS:.o=.d) $(WD3D_HOST_OBJS:.o=.d)

$(BUILD_DIR)/wd3d/dlls/%.c.o: $(WD3D_DIR)/dlls/%.c $(COMPILE_CONFIG) $(WD3D_FLAGS_FILE)
	@$(MKDIR_P) $(dir $@)
	@$(WD3D_CC) $(call wd3d_abs,$(WD3D_WINED3D_FLAGS)) -MMD -MP -c $< -o $@
$(BUILD_DIR)/wd3d/libs/%.c.o: $(WD3D_DIR)/libs/%.c $(COMPILE_CONFIG) $(WD3D_FLAGS_FILE)
	@$(MKDIR_P) $(dir $@)
	@$(WD3D_CC) $(call wd3d_abs,$(WD3D_VKD3D_FLAGS)) -MMD -MP -c $< -o $@
$(BUILD_DIR)/wd3d/shim/%.c.o: $(PROJDIR)/source/wined3d/%.c $(COMPILE_CONFIG) $(WD3D_FLAGS_FILE)
	@$(MKDIR_P) $(dir $@)
	@$(WD3D_CC) $(call wd3d_abs,$(WD3D_WINED3D_FLAGS)) -MMD -MP -c $< -o $@

endif
