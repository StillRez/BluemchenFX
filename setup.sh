#!/usr/bin/env bash
# One-time setup: initialise git, add libDaisy + DaisySP (incl. LGPL) as
# submodules and build both libraries. Run from Git Bash on Windows (the
# Daisy Toolchain installs make + arm-none-eabi-gcc there) or any Unix shell.
set -e
cd "$(dirname "$0")"

# Older copies of this project shipped the Makefile as Makefile.in
if [ ! -f Makefile ] && [ -f Makefile.in ]; then cp Makefile.in Makefile; fi

if [ ! -d .git ]; then
    git init
fi

if [ ! -f lib/libDaisy/Makefile ]; then
    git submodule add https://github.com/electro-smith/libDaisy lib/libDaisy || true
fi
if [ ! -f lib/DaisySP/Makefile ]; then
    git submodule add https://github.com/electro-smith/DaisySP lib/DaisySP || true
fi

git submodule update --init --recursive

# libDaisy is built size-optimised: its HAL code isn't performance-critical,
# and it keeps the legacy internal-flash build (APP_TYPE=BOOT_NONE) possible.
echo "Building libDaisy (OPT=-Os)..."
make -C lib/libDaisy clean
make -C lib/libDaisy -j4 OPT=-Os
echo "Building DaisySP (+ DaisySP-LGPL)..."
make -C lib/DaisySP -j4

echo "Done. Now run: make            (build firmware)"
echo "               make program-dfu (flash over USB, Daisy in DFU mode)"
