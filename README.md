# YabaSanshiro NX

A standalone Sega Saturn emulator for the Nintendo Switch, built on
[YabaSanshiro](https://github.com/devmiyax/yabause) by devMiyax: its ARM64 dynamic recompiler and
OpenGL 4.3 renderer run most games at full speed. It comes with its own launcher, adapted from
NaGa's [Dolphin NX](https://github.com/NaGaa95/dolphin-nx) frontend.

## Features

- **Full-speed emulation**: SH-2 dynarec on the Switch's JIT, GPU renderer with asynchronous
  rendering, up to 1080p when docked (switching between 720p and 1080p as you dock and undock).
- **Launcher**: cover-art library, favourites and collections, file manager, per-game settings,
  touch and controller navigation.
- **Game formats**: `.cue`/`.bin`, `.chd`, `.iso`, `.ccd`/`.img`, `.mds`/`.mdf`.
- **Storage**: SD card, USB drives (FAT, exFAT, NTFS) and SMB network shares.
- **Covers**: import your own, or download them from [SteamGridDB](https://www.steamgriddb.com)
  one game at a time or for the whole library.
- **HOME Menu shortcuts**: start a game straight from the Switch HOME Menu.
- **Quick menu** (Minus + Plus in game): 10 save-state slots, cheats, disc eject and change,
  FPS counter, console reset, return to the launcher.
- **Cheats**: Action Replay / GameShark codes, from RetroArch cheat files or typed in during a
  game (see [Cheats](#cheats)).
- **Controllers**, per player and per game: Saturn pad, 3D pad (analog), Virtual On Twin Stick or
  not connected, each with its own button mapping.
- **BIOS**: picks a Japanese or US/European BIOS by the disc's region, or one you choose per game;
  runs without a BIOS file too.
- **Settings**: global, with per-game overrides for emulation, video and controls.

## Requirements

- A Switch running custom firmware (Atmosphère) with the homebrew menu.
- **The CPU overclocked to 1785 MHz** (with sys-clk or a similar tool). At the stock clock many
  games won't hold full speed.
- HOME Menu shortcuts also need signature patches.
- A Saturn BIOS is recommended (see [BIOS](#bios)); without one the emulator uses its own
  high-level BIOS, which a few games don't like.

## Installing

1. Download `yabasanshiro.nro` from the [releases](../../releases).
2. Copy it to `switch/yabasanshiro/` on the SD card.
3. Put your games in `switch/yabasanshiro/games/` (subfolders are fine), and your BIOS files in
   `switch/yabasanshiro/bios/`.
4. Set the CPU to 1785 MHz, then start **YabaSanshiro NX** from the homebrew menu.

Everything the app keeps lives in `switch/yabasanshiro/`:

| Path | Contents |
|---|---|
| `games/` | Games (more folders, USB drives and SMB shares can be added in the launcher) |
| `bios/` | Saturn BIOS files |
| `states/` | Save states |
| `cheats/` | Cheat files |
| `covers/` | Cover art |
| `GameSettings/` | Per-game settings |
| `forwarders/` | HOME Menu shortcut configuration |
| `backup.bin` | The Saturn's internal and cartridge save memory |
| `settings.ini`, `input.ini` | Global settings and controls |
| `log.txt`, `log.prev.txt` | Logs of the current and previous run |

## BIOS

Put the BIOS files in `switch/yabasanshiro/bios/`. One Japanese and one US/European BIOS cover
every game; with the default **Auto** setting the emulator picks the one matching each disc's
region. A game can be set to a specific BIOS under **Game settings → Emulation → BIOS**.

| File | Version | Region | CRC32 |
|---|---|---|---|
| `sega_101.bin` | Japan v1.01 | Japanese | `224b752c` |
| `sega1003.bin` | Japan v1.003 | Japanese | `b3c63c25` |
| `sega_100.bin` | Japan v1.00 | Japanese | `2aba43c2` |
| `mpr-17933.bin` | Overseas v1.01a | US / Europe | `4afcf0fa` |
| `sega_100a.bin` | Overseas v1.00a | US / Europe | `f90f0089` |
| `vsaturn.bin` | JVC V-Saturn | Japanese | `e4d61811` |
| `mpr-18100.bin` | Hitachi HiSaturn v1.02 | Japanese | `3408dbf4` |
| `hisaturn.bin` | Hitachi HiSaturn v1.01 | Japanese | `721e1b60` |

`sega_101.bin` and `mpr-17933.bin` are the usual pair. Known dumps are recognised by their
contents whatever they are named; other 512 KB files are used by a region hint in the name
("jp", "us", "eu").

## Controls

Default Saturn pad mapping (changeable globally or per game under **Settings → Controls**):

| Saturn | Switch |
|---|---|
| D-pad | D-pad, left stick |
| A / B / C | B / A / R |
| X / Y / Z | Y / X / L |
| L / R | ZL / ZR |
| Start | Plus |

**Minus + Plus** opens the quick menu during a game.

Controller types:

- **Saturn pad**: the standard pad.
- **3D pad (analog)**: the NiGHTS pad in analog mode; the left stick is its analog stick and
  ZL / ZR its triggers. Only games made for it understand it, so set it per game.
- **Twin Stick (Virtual On)**: the two sticks act as the levers. Left lever = D-pad, left
  trigger / thumb = L / ZL, right lever up / down / left / right = Saturn Y / B / X / Z,
  right trigger / thumb = R / ZR; Switch Y fires the centre weapon.
- **Not connected**: an empty port, for games that wait for player 2 when a second controller
  is plugged in.

## Cheats

The quick menu's **Cheats** page lists the game's cheats: **A** turns one on or off, **Y** adds a
code with the keyboard (for example `1602E8F0 0063`; separate several codes with `+`) and
**X** twice deletes one. Changes are saved straight away.

Cheats are kept in RetroArch's `.cht` format in `switch/yabasanshiro/cheats/` (or its
`Sega - Saturn/` subfolder), so RetroArch's Saturn cheat files can be copied in as they are.
A file is found by the disc image's name (`Die Hard Arcade (USA).cht` for
`Die Hard Arcade (USA).chd`) or by the disc's product code (`MK-81814.cht`); the Cheats page
shows the names it looks for. Supported code types are `1` (16-bit write), `3` (8-bit write)
and `D` (only apply the next code while a value matches). Master codes aren't needed and are
ignored. Turning a cheat off stops it writing, but values it already changed stay until the
game changes them.

## Building

The build runs in Docker with devkitPro (devkitA64, libnx and the Switch portlibs, including
Mesa, SDL2, curl, turbojpeg and ntfs-3g):

```sh
yabause/src/nx/build_switch.sh
```

The result is `dist/switch/yabasanshiro/yabasanshiro.nro`; `build.log` has the compiler output.
`DKP_IMAGE` selects the Docker image. The Switch port's sources are in
[`yabause/src/nx`](yabause/src/nx).

## Credits

- **devMiyax** and the Yabause / YabaSanshiro contributors: the emulator, its dynarec and
  renderer, and the original Switch port.
- **NaGa**: the launcher and in-game menu, adapted from
  [Dolphin NX](https://github.com/NaGaa95/dolphin-nx) with his permission.
- [libsmb2](https://github.com/sahlberg/libsmb2) (Ronnie Sahlberg),
  [libusbhsfs](https://github.com/DarkMatterCore/libusbhsfs) (DarkMatterCore; the versions
  used are ITotalJustice's forks), [Dear ImGui](https://github.com/ocornut/imgui),
  [nx-hbloader](https://github.com/switchbrew/nx-hbloader) (the HOME Menu shortcut payload is
  derived from it), [libchdr](https://github.com/rtissera/libchdr), and devkitPro / switchbrew
  for the toolchain and libnx.

## License

GPL-2.0-or-later (see [LICENSE](LICENSE)). Bundled components keep their own licenses: libsmb2
(LGPL-2.1), libusbhsfs (ISC, NTFS support GPL-2.0-or-later), Dear ImGui (MIT), the HOME Menu
shortcut payload (ISC), and the shortcut installer (MPL-2.0); see the license files next to each.

Sega Saturn is a trademark of Sega. This project is not affiliated with Sega or Nintendo, and
includes no BIOS or game files.
