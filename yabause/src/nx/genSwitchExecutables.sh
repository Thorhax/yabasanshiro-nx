#!/bin/sh
# Runs in the nx build directory after linking; writes yabasanshiro.nro two levels up.
# $1: the nx source directory (for the icon and romfs)
# $2: app version
# $3: directory with the HOME Menu shortcut payload (hbl.nso, hbl.npdm, default.nacp)

set -o xtrace
set -e

SOURCE_DIR="$1"
APP_TITLE="YabaSanshiro NX"
APP_AUTHOR="Thorhax"
APP_VERSION="$2"
FORWARDER_DIR="$3"

# romfs: the launcher's assets plus the shortcut payload and the icon shortcuts start from
rm -rf romfs
cp -r "${SOURCE_DIR}/launcher/romfs" romfs
mkdir -p romfs/fwd
cp "${FORWARDER_DIR}/hbl.nso" "${FORWARDER_DIR}/hbl.npdm" "${FORWARDER_DIR}/default.nacp" romfs/fwd/
cp "${SOURCE_DIR}/icon.jpg" romfs/fwd/icon.jpg

mv -f yabasanshiro yabasanshiro.elf
nacptool --create "${APP_TITLE}" "${APP_AUTHOR}" "${APP_VERSION}" yabasanshiro.nacp
elf2nro yabasanshiro.elf ../../yabasanshiro.nro --nacp=yabasanshiro.nacp \
  --icon="${SOURCE_DIR}/icon.jpg" --romfsdir=romfs
