# Launcher

The launcher UI is adapted from NaGa's Dolphin NX frontend
(https://github.com/NaGaa95/dolphin-nx, `Source/Core/DolphinSwitch`), used with
the author's permission under GPL-2.0-or-later. `UPSTREAM` records the commit it
was taken from.

- `DolphinSwitch/` - the frontend sources, kept close to upstream so later
  improvements can be merged.
- `compat/` - small stand-ins for the Dolphin headers those sources include,
  implemented for YabaSanshiro (Saturn game files instead of GameCube/Wii ones).
- `romfs/` - assets bundled into the .nro.
- `ForwarderPayload/` - the program HOME Menu shortcuts run: a small nx-hbloader derivative
  that starts `yabasanshiro.nro` with the game's arguments (from Dolphin NX's
  `Tools/Switch/ForwarderPayload`, ISC). CMake builds it into romfs `fwd/`.

USB drives and SMB shares use the libraries in `../third_party`.
