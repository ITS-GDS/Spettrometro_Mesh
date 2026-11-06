#!/bin/bash
# Build con toolchain NCS. Percorsi sovrascrivibili dall'ambiente, es.:
#   NCS_TOOLCHAIN=/c/ncs/toolchains/<hash> ./build.sh
set -e
NCS_ROOT="${NCS_ROOT:-/c/ncs}"
NCS_VERSION="${NCS_VERSION:-v3.3.1}"
NCS_TOOLCHAIN="${NCS_TOOLCHAIN:-$NCS_ROOT/toolchains/936afb6332}"  # percorso senza spazi
BOARD="${BOARD:-xiao_ble}"
APP_DIR="$(cd "$(dirname "$0")" && pwd)"
win() { cygpath -m "$1"; }  # west/cmake vogliono percorsi C:/...

export PATH="$NCS_TOOLCHAIN/opt/bin:$NCS_TOOLCHAIN/opt/bin/Scripts:$NCS_TOOLCHAIN/bin:$NCS_TOOLCHAIN/mingw64/bin:$NCS_TOOLCHAIN/opt/zephyr-sdk/arm-zephyr-eabi/bin:$PATH"
export PYTHONHOME="$(win "$NCS_TOOLCHAIN/opt/bin")"
export ZEPHYR_BASE="$(win "$NCS_ROOT/$NCS_VERSION/zephyr")"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR="$(win "$NCS_TOOLCHAIN/opt/zephyr-sdk")"

cd "$NCS_ROOT/work"
west build -b "$BOARD" -d "$APP_DIR/build" --no-sysbuild "$APP_DIR" -p auto
