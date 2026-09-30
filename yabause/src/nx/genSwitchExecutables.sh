#!/bin/sh
# Runs in the nx build directory after linking; writes yabasanshiro.nro two levels up.
# $1: the nx source directory (for the icon and romfs)

set -o xtrace
set -e

SOURCE_DIR="$1"
APP_TITLE="YabaSanshiro NX"
APP_AUTHOR="devMiyax, Switch port"
APP_VERSION="0.2.0"

mv -f yabasanshiro yabasanshiro.elf
nacptool --create "${APP_TITLE}" "${APP_AUTHOR}" "${APP_VERSION}" yabasanshiro.nacp
elf2nro yabasanshiro.elf ../../yabasanshiro.nro --nacp=yabasanshiro.nacp \
  --icon="${SOURCE_DIR}/icon.jpg" --romfsdir="${SOURCE_DIR}/launcher/romfs"
