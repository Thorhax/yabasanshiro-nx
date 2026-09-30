set (CMAKE_SYSTEM_NAME Switch)
set (CMAKE_SYSTEM_VERSION 1)
set (CMAKE_SYSTEM_PROCESSOR aarch64)

if (NOT DEFINED DEVKITPRO)
	set (DEVKITPRO "/opt/devkitpro")
endif (NOT DEFINED DEVKITPRO)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE NEVER)

set( DEVKITPRO_BIN "${DEVKITPRO}/devkitA64/bin/" )
set( PREFIX "aarch64-none-elf-" )

# Tools
set(CMAKE_C_COMPILER  ${DEVKITPRO_BIN}${PREFIX}gcc )
set(CMAKE_CXX_COMPILER ${DEVKITPRO_BIN}${PREFIX}g++ )
set(CMAKE_ASM_COMPILER ${DEVKITPRO_BIN}${PREFIX}gcc )
set(CMAKE_ASM-ATT_COMPILER ${DEVKITPRO_BIN}${PREFIX}as )
set(CMAKE_ASM_COMPILER_AR ${DEVKITPRO_BIN}${PREFIX}gcc-ar CACHE FILEPATH "" FORCE)
set(CMAKE_ASM_COMPILER_RANLIB ${DEVKITPRO_BIN}${PREFIX}gcc-ranlib )

set(CMAKE_OBJCOPY ${DEVKITPRO_BIN}${PREFIX}objcopy )
set(CMAKE_STRIP ${DEVKITPRO_BIN}${PREFIX}strip )
set(CMAKE_NM ${DEVKITPRO_BIN}${PREFIX}nm )
set(CMAKE_AR ${DEVKITPRO_BIN}${PREFIX}gcc-ar CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB ${DEVKITPRO_BIN}${PREFIX}gcc-ranlib )
set(CMAKE_LINKER ${DEVKITPRO_BIN}${PREFIX}ld )

# This file is read several times per configure (try_compile, external projects),
# so only use the *_INIT variables, never append to the real flag variables.
set( ARCH "-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -D__SWITCH__" )

set (CMAKE_C_FLAGS_INIT "-g -Wall -O2 -ffunction-sections -fdata-sections ${ARCH} -isystem ${DEVKITPRO}/libnx/include -I${DEVKITPRO}/portlibs/switch/include")
set (CMAKE_CXX_FLAGS_INIT "${CMAKE_C_FLAGS_INIT}")
# Upstream C code predates GCC 14/15 defaults (C23, pointer type errors)
set (CMAKE_C_FLAGS_INIT "${CMAKE_C_FLAGS_INIT} -std=gnu17 -Wno-error=incompatible-pointer-types -Wno-error=implicit-function-declaration -Wno-error=int-conversion")
set (CMAKE_ASM_FLAGS_INIT "${ARCH}")
set (CMAKE_EXE_LINKER_FLAGS_INIT "-specs=${DEVKITPRO}/libnx/switch.specs -L${DEVKITPRO}/libnx/lib -L${DEVKITPRO}/portlibs/switch/lib")

set (CMAKE_C_FLAGS_RELEASE_INIT "-O2")
set (CMAKE_CXX_FLAGS_RELEASE_INIT "-O2")


add_definitions( -DNX )
