PROJECT       := apc_mini
BUILD_DIR     ?= build
BOOTLOADER    ?= 1

CROSS_COMPILE ?= arm-none-eabi-
CC            := $(CROSS_COMPILE)gcc
OBJCOPY       := $(CROSS_COMPILE)objcopy
OBJDUMP       := $(CROSS_COMPILE)objdump
SIZE          := $(CROSS_COMPILE)size
UV            ?= uv

OPENCM3_DIR   := lib/libopencm3
OPENCM3_LIB   := $(OPENCM3_DIR)/lib/libopencm3_stm32f1.a

INCLUDES := \
	-Iinclude \
	-Isrc \
	-Isrc/platform \
	-Isrc/drivers \
	-Isrc/usb \
	-Isrc/protocol \
	-I$(OPENCM3_DIR)/include

# -----------------------------------------------------------------------------
# USB Identity & Legal Notice:
# The default USB VID/PID (0x09E8:0x0028) and manufacturer/product strings are
# configured strictly for hardware interoperability on genuine retail Akai APC
# Mini controllers.
#
# DISCLAIMER: If you build or port this firmware for any custom, clone, or
# non-Akai hardware target, you MUST NOT use Akai's USB VID/PID (0x09E8:0x0028)
# or vendor name ("AKAI PROFESSIONAL,LP"). Instead, override these variables
# using one of the following legitimate options for open-source devices:
#   1. pid.codes (https://pid.codes): Request a free PID under VID 0x1209.
#   2. Objective Development shared USB-MIDI ID (VID 0x16C0, PID 0x05E4), with
#      USB_STRING_VENDOR set to your own domain or e-mail address, e.g.:
#        make USB_VID=0x16C0 USB_PID=0x05E4 \
#             USB_STRING_VENDOR="example.org" \
#             USB_STRING_PRODUCT="Open Grid Controller"
# -----------------------------------------------------------------------------
GIT_VERSION        := $(shell git describe --tags --exact-match 2>/dev/null || git rev-parse --short HEAD 2>/dev/null || echo dev)

USB_VID            ?= 0x09E8
USB_PID            ?= 0x0028
USB_STRING_VENDOR  ?= AKAI PROFESSIONAL,LP
USB_STRING_PRODUCT ?= APC MINI
USB_STRING_SERIAL  ?= $(GIT_VERSION)

ifeq ($(BOOTLOADER),0)
MODE_SUFFIX   := standalone
LDSCRIPT      := ld/stm32f102rb_standalone.ld
else
MODE_SUFFIX   := app
LDSCRIPT      := ld/stm32f102rb_app.ld
endif

DEFINES := \
	-DSTM32F1 \
	-DUSB_VID=$(USB_VID) \
	-DUSB_PID=$(USB_PID) \
	'-DUSB_STRING_VENDOR="$(USB_STRING_VENDOR)"' \
	'-DUSB_STRING_PRODUCT="$(USB_STRING_PRODUCT)"' \
	'-DUSB_STRING_SERIAL="$(USB_STRING_SERIAL)"'

CFLAGS := \
	-mcpu=cortex-m3 \
	-mthumb \
	-Oz \
	-g \
	-std=c11 \
	-Wall \
	-Wextra \
	-Wno-unused-parameter \
	-ffunction-sections \
	-fdata-sections \
	-fno-common \
	-MMD -MP \
	$(DEFINES) \
	$(INCLUDES)

APP_ELF       := $(BUILD_DIR)/$(PROJECT)_app.elf
APP_RAW_BIN   := $(BUILD_DIR)/$(PROJECT)_app_raw.bin
APP_BIN       := $(BUILD_DIR)/$(PROJECT)_app.bin
APP_SYX       := $(BUILD_DIR)/$(PROJECT)_app.syx

STANDALONE_ELF := $(BUILD_DIR)/$(PROJECT)_standalone.elf
STANDALONE_BIN := $(BUILD_DIR)/$(PROJECT)_standalone.bin

TARGET_ELF    := $(BUILD_DIR)/$(PROJECT)_$(MODE_SUFFIX).elf
TARGET_MAP    := $(BUILD_DIR)/$(PROJECT)_$(MODE_SUFFIX).map

LDFLAGS := \
	-mcpu=cortex-m3 \
	-mthumb \
	-L$(OPENCM3_DIR)/lib \
	-T$(LDSCRIPT) \
	-nostartfiles \
	-Wl,--gc-sections \
	-Wl,-Map=$(TARGET_MAP) \
	--specs=nano.specs \
	--specs=nosys.specs

LDLIBS := \
	-Wl,--start-group \
	-lopencm3_stm32f1 \
	-lc \
	-lgcc \
	-lnosys \
	-Wl,--end-group

APP_SRCS := \
	src/main.c \
	src/platform/isr.c \
	src/platform/board.c \
	src/platform/bootloader.c \
	src/drivers/tm1629.c \
	src/drivers/leds.c \
	src/drivers/buttons.c \
	src/drivers/faders.c \
	src/usb/usb_desc.c \
	src/usb/usb_midi.c \
	src/protocol/sysex.c \
	src/protocol/apc_mode.c \
	src/protocol/monome_mode.c \
	src/protocol/lua_vm.c

OBJS := $(patsubst src/%.c,$(BUILD_DIR)/obj/%.o,$(APP_SRCS))

WEB_DIST_DIR  := $(BUILD_DIR)/web
WEB_PORT      ?= 8000

.PHONY: all sysex verify validate test-validator flash lua-wasm web-dist serve-web disasm clean clean-all

ifeq ($(BOOTLOADER),0)
all: $(STANDALONE_BIN)
else
all: $(APP_BIN) $(APP_SYX)
endif

sysex: $(APP_SYX)

$(OPENCM3_LIB):
	$(MAKE) -C $(OPENCM3_DIR) TARGETS=stm32/f1 PREFIX="$(CROSS_COMPILE)" CFLAGS="-Oz"

$(BUILD_DIR)/obj/%.o: src/%.c | $(OPENCM3_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -c $< -o $@

$(BUILD_DIR)/obj/protocol/lua_vm.o: EXTRA_CFLAGS := -O2

$(TARGET_ELF): $(OBJS) $(EXTRA_OBJS) $(OPENCM3_LIB) $(LDSCRIPT)
	$(CC) $(OBJS) $(EXTRA_OBJS) $(LDFLAGS) $(EXTRA_LDFLAGS) -o $@ $(LDLIBS)
	$(SIZE) $@

$(APP_RAW_BIN): $(APP_ELF)
	$(OBJCOPY) -O binary $< $@

$(APP_BIN): $(APP_RAW_BIN) tools/stamp_fw.py
	$(UV) run stamp_fw $(APP_RAW_BIN) $(APP_BIN)

$(APP_SYX): $(APP_BIN) tools/bin_to_sysex.py
	$(UV) run bin_to_sysex $(APP_BIN) $(APP_SYX)

$(STANDALONE_BIN): $(STANDALONE_ELF)
	$(OBJCOPY) -O binary $< $@

verify validate: $(APP_BIN) $(APP_SYX)
	$(UV) run validate_fw $(APP_BIN)

test-validator: $(APP_BIN) $(APP_SYX)
	$(UV) run test_validator

flash: $(APP_SYX)
	$(UV) run fw_updater $(APP_SYX)

debug:
	$(UV) run fw_debug

lua-wasm:
	@if command -v cargo >/dev/null 2>&1 && rustup target list --installed 2>/dev/null | grep -q wasm32-unknown-unknown; then \
		cargo build --manifest-path tools/lua_wasm/Cargo.toml --target wasm32-unknown-unknown --release && \
		cp tools/lua_wasm/target/wasm32-unknown-unknown/release/apc_lua_compiler.wasm web/apc_lua_compiler.wasm; \
	else \
		echo "[WASM] Using prebuilt web/apc_lua_compiler.wasm"; \
	fi

web-dist: $(APP_BIN) $(APP_SYX) lua-wasm $(wildcard web/*) $(wildcard scripts/*.lua) tools/build_web_manifest.py
	$(UV) run build_web_manifest --web-src web --out-dir $(WEB_DIST_DIR) --bin $(APP_BIN) --syx $(APP_SYX) --elf $(APP_ELF)

serve-web: web-dist
	@echo "Opening WebMIDI Updater at http://localhost:$(WEB_PORT) (localhost is a W3C Secure Context)"
	python3 -m http.server --bind 127.0.0.1 -d $(WEB_DIST_DIR) $(WEB_PORT)

disasm: $(TARGET_ELF)
	$(OBJDUMP) -d -S $(TARGET_ELF) > $(BUILD_DIR)/$(PROJECT)_$(MODE_SUFFIX).lst

clean:
	rm -rf $(BUILD_DIR)

clean-all: clean
	$(MAKE) -C $(OPENCM3_DIR) clean

-include $(OBJS:.o=.d)
