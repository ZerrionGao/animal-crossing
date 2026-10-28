ENGINE := ../../engine
OUT := ../../work/acnh/3.0.0
OUT_ABS := $(abspath $(OUT))
PY := ../../.venv/bin/python
NATIVE_OPT ?= -O2

GFX_SRC := $(wildcard $(ENGINE)/graphics/nvn/*.c $(ENGINE)/graphics/nvn/*.m $(ENGINE)/graphics/nvn/*.S $(ENGINE)/graphics/texture/*.c $(ENGINE)/graphics/texture/*.m)
GFX_OBJECTS := $(patsubst $(ENGINE)/%,$(OUT)/build/acnh-gfx/%.o,$(GFX_SRC))
GFX_HEADERS := $(wildcard $(ENGINE)/graphics/nvn/*.h $(ENGINE)/graphics/nvn/include/*.h $(ENGINE)/graphics/host/*.h) runtime/acnh_msl_normalize.h
GFX_FLAGS := -std=gnu11 -Wno-error -Wno-unused-function -Wno-unused-parameter -DGAME_ACNH -fobjc-arc -I$(ENGINE)/graphics/nvn/include -I$(abspath runtime)

RUNTIME_COMMON := $(addprefix $(ENGINE)/runtime/,loader.c game_patches.c game_slot.c services.c romfs.c romfs_directory.c threads.c gate.S hle_stack.S hle_stack.c thread_enter.S gfx_bridge.c frontend.m account.c input.c hid_feedback.c motion_fusion.c autoload.c gamepad.m gamepad_feedback.m gfx_probe.c vpool_probe.m gpu_path_probe.m memory_probe.c ipc.c nifm_offline.c friends_offline.c bcat_offline.c ngc_aho.c ngc_service.c olsc_offline.c pctl_offline.c settings_service.c audio_renderer.c audio_mixer.c audio_reverb.c audio_effects.c fs_host.c opus_codec.c movie_host.c audout_host.c time_host.c applet_host.c)
RUNTIME_HEADERS := $(wildcard $(ENGINE)/runtime/*.h)
RUNTIME := $(RUNTIME_COMMON) runtime/safety_stubs.c runtime/shared_font_stub.c
RUNTIME_BOOT := $(RUNTIME_COMMON) runtime/safety_stubs.c $(ENGINE)/runtime/shared_font.c
ACNH_NORMALIZER := runtime/acnh_msl_normalize.c
LIBS := /opt/homebrew/opt/opus/lib/libopus.a /opt/homebrew/opt/libvpx/lib/libvpx.a -lz
FRAMEWORKS := -framework Metal -framework MetalFX -framework Foundation -framework QuartzCore -framework ImageIO -framework CoreGraphics -framework CoreText -framework UniformTypeIdentifiers -framework Cocoa -framework CoreVideo -framework CoreFoundation -framework AudioToolbox -framework GameController -framework CoreHaptics -framework IOKit

.PHONY: help analyze runtime-tables runtime probe probe-gpu-midclear probe-gpu-ds-clear probe-gpu-acnh-msl boot-check boot app acnh-msl-normalizer acnh-msl-test
help:
	@echo 'analyze：只读检查入口、SDK 和构造函数指令证据'
	@echo 'runtime-tables：改写完成后生成并校验装载表'
	@echo 'runtime：编译仅开放跨模块探针的 ACNH 装载器'
	@echo 'probe：运行不进入游戏的跨模块装载探针'
	@echo 'boot：通过结构校验后编译并启动 ACNH（先运行 make boot-check）'
	@echo 'app：打成本机离线 Mac App 原型（不含 keys，不签名上架）'

analyze:
	$(PY) scripts/prepare_runtime.py analyze

$(OUT)/build/runtime_tables.h: scripts/prepare_runtime.py $(OUT)/rewritten/manifest.json
	$(PY) scripts/prepare_runtime.py tables --output $@

$(OUT)/build/shared_fonts_config.h: scripts/prepare_runtime.py profiles/shared-fonts.json $(OUT)/shared-fonts/manifest.json
	$(PY) scripts/prepare_runtime.py fonts-config

runtime-tables: $(OUT)/build/runtime_tables.h

$(OUT)/build/acnh-gfx/%.o: $(ENGINE)/% $(GFX_HEADERS) Makefile
	@mkdir -p "$(dir $@)"
	xcrun clang $(NATIVE_OPT) -g -Wall -Wextra $(GFX_FLAGS) -c "$<" -o "$@"

$(OUT)/build/acnh-native: Makefile $(RUNTIME) $(RUNTIME_HEADERS) $(GFX_OBJECTS) $(ACNH_NORMALIZER) $(OUT)/build/runtime_tables.h
	xcrun clang -std=c11 $(NATIVE_OPT) -g -Wall -Wextra -Werror -Wno-deprecated-declarations -DGAME_ACNH -I$(abspath runtime) -I$(ENGINE)/runtime -I$(ENGINE)/graphics/nvn/include -I$(OUT)/build -I/opt/homebrew/opt/opus/include/opus -I/opt/homebrew/opt/libvpx/include $(RUNTIME) $(ACNH_NORMALIZER) $(GFX_OBJECTS) $(LIBS) $(FRAMEWORKS) -fobjc-arc -o "$@"

runtime: $(OUT)/build/acnh-native

acnh-msl-normalizer: $(OUT)/build/acnh-msl-normalizer

acnh-msl-test:
	@mkdir -p "$(OUT)/build"
	$(CC) -std=c11 -Wall -Wextra -Werror ../../tests/acnh_msl_normalize_test.c runtime/acnh_msl_normalize.c -o "$(OUT)/build/acnh-msl-normalize-test"
	"$(OUT)/build/acnh-msl-normalize-test"
	xcrun clang -fobjc-arc -DGAME_ACNH -I$(ENGINE)/graphics/nvn ../../tests/acnh_shader_seed_policy_probe.m -framework Foundation -o "$(OUT)/build/acnh-shader-seed-policy-probe"
	"$(OUT)/build/acnh-shader-seed-policy-probe"
	$(PY) ../../tests/test_acnh_msl_normalize.py

$(OUT)/build/acnh-msl-normalizer: tools/acnh_msl_normalize.c runtime/acnh_msl_normalize.c runtime/acnh_msl_normalize.h
	@mkdir -p "$(dir $@)"
	$(CC) -std=c11 -Wall -Wextra -Werror tools/acnh_msl_normalize.c runtime/acnh_msl_normalize.c -o "$@"

boot-check:
	$(PY) scripts/prepare_runtime.py boot-check

$(OUT)/build/acnh-native-boot: Makefile $(RUNTIME_BOOT) $(RUNTIME_HEADERS) $(GFX_OBJECTS) $(ACNH_NORMALIZER) $(OUT)/build/runtime_tables.h boot-check $(OUT)/build/shared_fonts_config.h
	xcrun clang -std=c11 $(NATIVE_OPT) -g -Wall -Wextra -Werror -Wno-deprecated-declarations -DGAME_ACNH -DACNH_BOOT_VERIFIED=1 -I$(abspath runtime) -I$(ENGINE)/runtime -I$(ENGINE)/graphics/nvn/include -I$(OUT)/build -I/opt/homebrew/opt/opus/include/opus -I/opt/homebrew/opt/libvpx/include $(RUNTIME_BOOT) $(ACNH_NORMALIZER) $(GFX_OBJECTS) $(LIBS) $(FRAMEWORKS) -fobjc-arc -o "$@"

app: $(OUT)/build/acnh-native-boot
	$(PY) scripts/package_macos.py

boot: $(OUT)/build/acnh-native-boot
	TOTK_ROMFS=$(OUT_ABS)/romfs \
	TOTK_ROMFS_PATCHES=0 \
	TOTK_ROMFS_MODS=0 \
	TOTK_PROFILE_DIR=$(OUT_ABS)/test-profile \
	TOTK_SHADER_CACHE=$(OUT_ABS)/shader-cache \
	TOTK_SHADER_SEED_DIR=$(OUT_ABS)/build/shader-seed \
	TOTK_UNCAPPED=0 \
	DM_PRESENT_HZ=60 \
	DM_PRESENT_LINK=0 \
	DM_PRESENT_INTERVAL=game \
	TOTK_MFX=0 \
	TOTK_GPU_KEEPALIVE=0 \
	TOTK_NGC_DIR=$(OUT_ABS)/../../research/ngword2-from-xci-20261023/romfs \
	$(OUT_ABS)/build/acnh-native-boot $(OUT_ABS)/rewritten boot

probe: runtime
	$(OUT)/build/acnh-native $(OUT)/rewritten probe

probe-gpu-midclear probe-gpu-ds-clear: runtime
	@mkdir -p "$(OUT_ABS)/analysis"
	MTL_DEBUG_LAYER=1 MTL_DEBUG_LAYER_ERROR_MODE=assert TOTK_HEADLESS=1 \
		$(OUT_ABS)/build/acnh-native $(OUT_ABS)/rewritten $@ > "$(OUT_ABS)/analysis/native-$@.json"
	cat "$(OUT_ABS)/analysis/native-$@.json"

probe-gpu-acnh-msl:
	@mkdir -p "$(OUT)/build"
	xcrun clang -fobjc-arc -DGAME_ACNH ../../tests/acnh_msl_metal_probe.m runtime/acnh_msl_normalize.c -framework Metal -framework Foundation -o "$(OUT)/build/acnh-msl-metal-probe"
	"$(OUT)/build/acnh-msl-metal-probe"
