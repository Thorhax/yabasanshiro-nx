# Third-party libraries

Built by `build_switch.sh` as a separate CMake project (`CMakeLists.txt` here) with devkitPro's
Switch toolchain, then linked into the app. Both are the revisions NaGa's Dolphin NX pins.

- `libsmb2/` - SMB2/3 client used for network shares.
  https://github.com/ITotalJustice/libsmb2 at 867beea093f2863dfddea01945204f724afd6c45,
  LGPL-2.1 (`LICENCE-LGPL-2.1.txt`). Examples, tests and other platforms' files are left out.
- `libusbhsfs/` - USB mass storage (FAT, exFAT, NTFS via switch-ntfs-3g).
  https://github.com/ITotalJustice/libusbhsfs at 625269b7725a6e2a3f2724e8d45b602c1b20ead5,
  ISC (`LICENSE_ISC.md`; the NTFS code is GPLv2+, `LICENSE_GPLv2+.md`).
  `uasp/` holds Dolphin NX's UASP transport (`usbhsfs_uasp.c/h`) and the three patches it
  needs, which are already applied to `source/`.
