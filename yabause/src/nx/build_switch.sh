#!/bin/bash
# Builds the YabaSanshiro Switch port (yabause/src/nx) inside a devkitPro docker image.
#
# Usage: yabause/src/nx/build_switch.sh [extra cmake args...]
#
# Output, relative to the repository root:
#   build/                                 CMake build tree, build.log has the full compiler output
#   dist/switch/yabasanshiro/yabasanshiro.nro
#                                          copy the contents of dist/ to the root of the SD card
#
# DKP_IMAGE selects the image: it needs devkitA64, libnx and the switch portlibs used here
# (SDL2, SDL2_ttf, SDL2_image, mesa, glad, libdrm_nouveau, freetype, harfbuzz, png, jpeg, webp,
# curl, turbojpeg, ntfs-3g).
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
DKP_IMAGE="${DKP_IMAGE:-devkitpro-mesa-rust:latest}"
SRC=/work/yabause
BUILD=/work/build

docker run --rm -v "${REPO_DIR}:/work" -w /work "${DKP_IMAGE}" bash -c "
set -e
export DEVKITPRO=/opt/devkitpro
export DEVKITA64=/opt/devkitpro/devkitA64
export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:\$PATH
mkdir -p ${BUILD}
# USB and SMB storage libraries, with devkitPro's own Switch toolchain
if [ ! -f ${BUILD}/third_party/install/lib/libsmb2.a ] || [ ! -f ${BUILD}/third_party/install/lib/liblibusbhsfs.a ]; then
  cmake -S ${SRC}/src/nx/third_party -B ${BUILD}/third_party \
    -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=${BUILD}/third_party/install > /work/build-third-party.log 2>&1
  cmake --build ${BUILD}/third_party -j\$(nproc) >> /work/build-third-party.log 2>&1 || { tail -40 /work/build-third-party.log; exit 1; }
  cmake --install ${BUILD}/third_party >> /work/build-third-party.log 2>&1
fi
cd ${BUILD}
cmake ${SRC} \
  -DCMAKE_TOOLCHAIN_FILE=${SRC}/src/nx/nx-toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DYAB_PORTS=nx \
  -DYAB_WANT_DYNAREC_DEVMIYAX=ON \
  -DSH2_DYNAREC=OFF \
  -DYAB_WANT_VULKAN=OFF \
  -DYAB_WANT_OPENAL=OFF \
  -DYAB_WANT_OPENSL=OFF \
  -DSDL2_INCLUDE_DIR=/opt/devkitpro/portlibs/switch/include/SDL2 \
  -DSDL2_LIBRARY=/opt/devkitpro/portlibs/switch/lib/libSDL2.a \
  -DYAB_NETWORK=OFF \
  -DYAB_USE_SSF=OFF \
  -DSH2_TRACE=OFF \
  $*
make -j\$(nproc) > /work/build.log 2>&1 || { grep -E ' error: |undefined reference|Error [0-9]' /work/build.log | head -40; exit 1; }
"

DIST="${REPO_DIR}/dist/switch/yabasanshiro"
mkdir -p "${DIST}"
cp "${REPO_DIR}/build/yabasanshiro.nro" "${DIST}/yabasanshiro.nro"
echo "Built: ${DIST}/yabasanshiro.nro"
