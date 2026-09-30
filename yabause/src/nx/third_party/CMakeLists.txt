# USB and SMB storage libraries for the launcher, built as their own project with devkitPro's
# Switch toolchain (their build files look for CMAKE_SYSTEM_NAME NintendoSwitch, which the
# emulator's toolchain doesn't set). build_switch.sh builds and installs this before the
# emulator; see README.md for where each library comes from.
cmake_minimum_required(VERSION 3.13)
project(yabasanshiro_nx_third_party C)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBKRB5 OFF CACHE BOOL "" FORCE)
set(ENABLE_GSSAPI OFF CACHE BOOL "" FORCE)
add_subdirectory(libsmb2)
# As in NaGa's Dolphin NX build (which also supplies its own readv/writev; here libsmb2's
# fallbacks for platforms without them are used)
target_compile_definitions(smb2 PRIVATE AES128_ECB_encrypt=smb2_AES128_ECB_encrypt)

# FAT and exFAT, plus NTFS (switch-ntfs-3g); EXT2/3/4 stays off
set(USBHSFS_GPL OFF CACHE BOOL "" FORCE)
set(USBHSFS_NTFS ON CACHE BOOL "" FORCE)
set(USBHSFS_EXT4 OFF CACHE BOOL "" FORCE)
set(USBHSFS_SXOS_DISABLE ON CACHE BOOL "" FORCE)
set(USBHSFS_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(libusbhsfs)
# UASP transport from NaGa's build (libusbhsfs/uasp; the source patches there are already
# applied to libusbhsfs/source)
target_sources(libusbhsfs PRIVATE libusbhsfs/uasp/usbhsfs_uasp.c)
target_include_directories(libusbhsfs PRIVATE libusbhsfs/uasp libusbhsfs/source)

install(TARGETS smb2 libusbhsfs ARCHIVE DESTINATION lib)
install(DIRECTORY libsmb2/include/smb2 DESTINATION include)
install(FILES libusbhsfs/include/usbhsfs.h DESTINATION include)
