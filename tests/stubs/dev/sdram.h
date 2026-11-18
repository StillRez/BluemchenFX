#pragma once
// Host stand-in for libDaisy's dev/sdram.h: on the PC there's no SDRAM
// section, so SDRAM buffers become ordinary globals.
#define DSY_SDRAM_BSS
