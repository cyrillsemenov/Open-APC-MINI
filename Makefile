PROJECT       := apc_mini
BUILD_DIR     ?= build

CROSS_COMPILE ?= arm-none-eabi-
CC            := $(CROSS_COMPILE)gcc
OBJCOPY       := $(CROSS_COMPILE)objcopy
OBJDUMP       := $(CROSS_COMPILE)objdump
SIZE          := $(CROSS_COMPILE)size
UV            ?= uv

STDPERIPH_DIR := lib/STM32F10x_StdPeriph_Lib/Libraries
CMSIS_CORE    := $(STDPERIPH_DIR)/CMSIS/CM3/CoreSupport
CMSIS_DEV     := $(STDPERIPH_DIR)/CMSIS/CM3/DeviceSupport/ST/STM32F10x
STDPERIPH_INC := $(STDPERIPH_DIR)/STM32F10x_StdPeriph_Driver/inc
STDPERIPH_SRC := $(STDPERIPH_DIR)/STM32F10x_StdPeriph_Driver/src
USB_DRV_DIR   := lib/STM32_USB-FS-Device_Driver
USB_DRV_INC   := $(USB_DRV_DIR)/inc
USB_DRV_SRC   := $(USB_DRV_DIR)/src

INCLUDES := \
	-Iinclude \
	-Iinclude/board \
	-Iinclude/platform \
	-Iinclude/usb \
	-Isrc \
	-Isrc/board \
	-Isrc/platform \
	-Isrc/usb \
	-I$(CMSIS_CORE) \
	-I$(CMSIS_DEV) \
	-I$(STDPERIPH_INC) \
	-I$(USB_DRV_INC)

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

DEFINES := \
	-DSTM32F10X_MD \
	-DUSE_STDPERIPH_DRIVER \
	-DUSB_VID=$(USB_VID) \
	-DUSB_PID=$(USB_PID) \
	'-DUSB_STRING_VENDOR="$(USB_STRING_VENDOR)"' \
	'-DUSB_STRING_PRODUCT="$(USB_STRING_PRODUCT)"' \
	'-DUSB_STRING_SERIAL="$(USB_STRING_SERIAL)"'

CFLAGS := \
	-mcpu=cortex-m3 \
	-mthumb \
	-Os \
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

LDFLAGS := \
	-mcpu=cortex-m3 \
	-mthumb \
	-Tld/stm32f102rb_app.ld \
	-nostartfiles \
	-Wl,--gc-sections \
	-Wl,-Map=$(BUILD_DIR)/$(PROJECT)_app.map \
	--specs=nano.specs \
	--specs=nosys.specs

APP_SRCS := \
	src/main.c \
	src/platform/startup_stm32f10x_md.c \
	src/platform/stm32f10x_it.c \
	src/board/hw_config.c \
	src/board/leds_tm1629.c \
	src/board/buttons_faders.c \
	src/usb/usb_desc.c \
	src/usb/usb_prop.c \
	src/usb/usb_pwr.c \
	src/usb/usb_istr.c \
	src/usb/usb_endp.c \
	src/usb/usb_midi_io.c

STDPERIPH_SRCS := \
	$(STDPERIPH_SRC)/misc.c \
	$(STDPERIPH_SRC)/stm32f10x_adc.c \
	$(STDPERIPH_SRC)/stm32f10x_dma.c \
	$(STDPERIPH_SRC)/stm32f10x_flash.c \
	$(STDPERIPH_SRC)/stm32f10x_gpio.c \
	$(STDPERIPH_SRC)/stm32f10x_iwdg.c \
	$(STDPERIPH_SRC)/stm32f10x_rcc.c

USB_SRCS := \
	$(USB_DRV_SRC)/usb_core.c \
	$(USB_DRV_SRC)/usb_init.c \
	$(USB_DRV_SRC)/usb_int.c \
	$(USB_DRV_SRC)/usb_mem.c \
	$(USB_DRV_SRC)/usb_regs.c \
	$(USB_DRV_SRC)/usb_sil.c

OBJS := \
	$(patsubst src/%.c,$(BUILD_DIR)/app/%.o,$(APP_SRCS)) \
	$(patsubst $(STDPERIPH_SRC)/%.c,$(BUILD_DIR)/stdperiph/%.o,$(STDPERIPH_SRCS)) \
	$(patsubst $(USB_DRV_SRC)/%.c,$(BUILD_DIR)/usb/%.o,$(USB_SRCS))

APP_ELF       := $(BUILD_DIR)/$(PROJECT)_app.elf
APP_RAW_BIN   := $(BUILD_DIR)/$(PROJECT)_app_raw.bin
APP_BIN       := $(BUILD_DIR)/$(PROJECT)_app.bin
APP_SYX       := $(BUILD_DIR)/$(PROJECT)_app.syx
WEB_DIST_DIR  := $(BUILD_DIR)/web
WEB_PORT      ?= 8000

.PHONY: all sysex verify validate test-validator flash web-dist serve-web disasm clean

all: $(APP_BIN) $(APP_SYX)

sysex: $(APP_SYX)

$(BUILD_DIR)/app/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -c $< -o $@

$(BUILD_DIR)/stdperiph/%.o: $(STDPERIPH_SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -c $< -o $@

$(BUILD_DIR)/usb/%.o: $(USB_DRV_SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -c $< -o $@

$(APP_ELF): $(OBJS) $(EXTRA_OBJS) ld/stm32f102rb_app.ld
	$(CC) $(OBJS) $(EXTRA_OBJS) $(LDFLAGS) $(EXTRA_LDFLAGS) -o $@
	$(SIZE) $@

$(APP_RAW_BIN): $(APP_ELF)
	$(OBJCOPY) -O binary $< $@

$(APP_BIN): $(APP_RAW_BIN) tools/stamp_fw.py
	$(UV) run stamp_fw $(APP_RAW_BIN) $(APP_BIN)

$(APP_SYX): $(APP_BIN) tools/bin_to_sysex.py
	$(UV) run bin_to_sysex $(APP_BIN) $(APP_SYX)

verify validate: $(APP_BIN) $(APP_SYX)
	$(UV) run validate_fw $(APP_BIN)

test-validator: $(APP_BIN) $(APP_SYX)
	$(UV) run test_validator

flash: $(APP_SYX)
	$(UV) run fw_updater $(APP_SYX)

web-dist: $(APP_BIN) $(APP_SYX) web/index.html tools/build_web_manifest.py
	$(UV) run build_web_manifest --web-src web --out-dir $(WEB_DIST_DIR) --bin $(APP_BIN) --syx $(APP_SYX)

serve-web: web-dist
	@echo "Opening WebMIDI Updater at http://localhost:$(WEB_PORT) (localhost is a W3C Secure Context)"
	python3 -m http.server --bind 127.0.0.1 -d $(WEB_DIST_DIR) $(WEB_PORT)

disasm: $(APP_ELF)
	$(OBJDUMP) -d -S $(APP_ELF) > $(BUILD_DIR)/$(PROJECT)_app.lst

clean:
	rm -rf $(BUILD_DIR)
