# BluemchenFX — Daisy Seed effect firmware for the kxmx Bluemchen
TARGET = BluemchenFX

CPP_SOURCES = \
	src/main.cpp \
	src/hw/bluemchen.cpp \
	src/ui/ui.cpp \
	src/fx/reverb.cpp \
	src/fx/delay.cpp \
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

# Size-optimised to fit the 128 KB internal flash (DSP files opt back in
# to -O2 with #pragma GCC optimize). libDaisy must also be built with
# OPT=-Os, which setup.sh does.
OPT = -Os

# Core location, and generic Makefile.
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

# Always (re)build before flashing: libDaisy's program targets only flash
# whatever is already in build/, which silently flashes stale firmware.
program-dfu program program-boot: all
