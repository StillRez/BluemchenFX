# Minimal BOOT_SRAM test program (see main.cpp)
TARGET = bootcheck
CPP_SOURCES = main.cpp
LIBDAISY_DIR = ../../lib/libDaisy
APP_TYPE = BOOT_SRAM
OPT = -O2
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
program-dfu: all
