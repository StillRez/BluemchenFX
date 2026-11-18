# BluemchenFX — Daisy Seed effect firmware for the kxmx Bluemchen
TARGET = BluemchenFX

CPP_SOURCES = \
	src/main.cpp \
	src/hw/bluemchen.cpp \
	src/ui/ui.cpp \
	src/fx/reverb.cpp \
	src/fx/delay.cpp \
	src/fx/registry.cpp \
	src/app/chain.cpp \
	src/app/chain_settings.cpp \
	src/app/preset_text.cpp \
	src/app/preset_store.cpp

C_INCLUDES = -Isrc

# Library locations (git submodules, see setup.sh)
LIBDAISY_DIR = lib/libDaisy
DAISYSP_DIR  = lib/DaisySP

# SD card presets
USE_FATFS = 1

# ReverbSc lives in DaisySP-LGPL
USE_DAISYSP_LGPL = 1

# Runs via the Daisy bootloader: the program is stored in QSPI and copied to
# the 480 KB AXI SRAM at boot (variables/stack in the 128 KB DTCM). Install
# the bootloader once with `make program-boot` (Daisy in DFU mode:
# hold BOOT, tap RESET), then flash the app with `make program-dfu` during
# the bootloader's grace period (tap RESET, then tap BOOT: LED keeps pulsing).
#
# Legacy internal-flash build (128 KB, no bootloader), while it still fits:
#   make APP_TYPE=BOOT_NONE OPT=-Os
APP_TYPE ?= BOOT_SRAM
OPT      ?= -O2

# Boot diagnostics: `make DIAG=1 BUILD_DIR=build-diag` blinks the Seed LED 3x
# right after the chip is up and shows each startup step on the OLED.
ifeq ($(DIAG),1)
C_DEFS += -DBOOT_DIAG
endif

# Core location, and generic Makefile.
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

# Always (re)build before flashing the app: libDaisy's program targets only
# flash whatever is already in build/, which silently flashes stale firmware.
# (`program-boot` installs the bootloader itself and needs no build.)
program-dfu program: all
