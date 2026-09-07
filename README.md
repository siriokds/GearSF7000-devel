# GearSF7000-devel

GearSF7000 is a cross-platform emulator and development environment for the
Sega SF-7000, SC-3000, SR-1000 and SP-400, written in C++.

This repository contains the development sources. It follows the layout used
by the other Gear emulators: the emulator core is in `src`, while desktop,
libretro and OpenEmu front ends live in `platforms`.

GearSF7000-devel is based on a version of
[GearColeco](https://github.com/drhelius/Gearcoleco) by
[Ignacio Sánchez Gines (drhelius)](https://github.com/drhelius), adapted for
the SF-7000 family of machines. Its Z80 CPU implementation uses
[CLK](https://github.com/TomHarte/CLK) by [Tom Harte](https://github.com/TomHarte),
adapted to the project requirements, including a reduced emulation stack.

## Technical overview

- **Language and toolchain:** C++20 for the desktop front ends, C99 for C
  sources, and Visual Studio on Windows.
- **Desktop runtime:** SDL3 with the SDL GPU renderer; GTK3 is used for native
  file dialogs on Linux and AppKit on macOS.
- **Interface:** Dear ImGui and ImPlot, with debugger, disassembler, memory
  editor, I/O inspection, VRAM views and rewind support.
- **Emulation core:** CLK Z80 processor, TMS9918 video display processor,
  SN76489 sound generator and optional AY-3-8910 / YM2149 expansion audio.
- **Hardware:** SF-7000 floppy subsystem, SR-1000 cassette recorder and
  SK-1100 keyboard support. SP-400 printer support is planned.
- **Targets:** standalone desktop builds for macOS, Linux and Windows;
  GearSC3000 cores for Libretro and OpenEmu.

Desktop release packages include their runtime assets. The macOS application
bundle contains the keyboard image and fonts; Windows embeds them in the
executable; Linux packages distribute them next to the executable.

## Third-party components and credits

GearSF7000-devel incorporates or builds against the following external
projects. Their included licence files and notices remain applicable.

- [GearColeco](https://github.com/drhelius/Gearcoleco), by Ignacio Sánchez
  Gines (drhelius), is the upstream base for this project.
- [Clock Signal (CLK)](https://github.com/TomHarte/CLK), by Tom Harte,
  provides the imported Z80 subset (MIT). The imported revision is recorded
  in [`third_party/clk/README-GearSF7000.md`](third_party/clk/README-GearSF7000.md).
- [blueMSX](http://www.bluemsx.com/), by Daniel Vik and Tomas Karlsson,
  provides the adapted floppy-controller and disk support (GPL-2.0-or-later).
- [Game Music Emu](https://bitbucket.org/mpyne/game-music-emu/) and
  [Blip_Buffer](https://www.slack.net/~ant/), by Shay Green, provide the
  audio emulation and resampling code (LGPL-2.1-or-later).
- [miniz](https://github.com/richgel999/miniz), by Rich Geldreich, provides
  ZIP and deflate support (public domain).
- [dr_libs](https://github.com/mackron/dr_libs), by David Reid, provides MP3
  decoding through `dr_mp3` (public domain or MIT-0).
- [SDL3](https://github.com/libsdl-org/SDL), maintained by the SDL project,
  provides the cross-platform desktop runtime (zlib licence).
- [Dear ImGui](https://github.com/ocornut/imgui), by Omar Cornut and
  contributors, provides the user interface (MIT).
- [ImPlot](https://github.com/epezent/implot), by Evan Pezent and Breno Cunha
  Queiroz, provides plotting support (MIT).
- [Native File Dialog Extended](https://github.com/btzy/nativefiledialog-extended),
  by Bernard Teo and Michael Labbe, provides native file dialogs (zlib).
- [mINI](https://github.com/pulzed/mINI), by Danijel Durakovic, provides INI
  file handling (MIT); [stb](https://github.com/nothings/stb), by Sean
  Barrett and contributors, provides image loading and writing (public domain
  or MIT).
- [JSON for Modern C++](https://github.com/nlohmann/json), by Niels Lohmann
  and contributors, provides JSON support (MIT).
- [SDL_GameControllerDB](https://github.com/gabomdq/SDL_GameControllerDB)
  supplies the bundled controller mapping database.
- [Libretro](https://www.libretro.com/) and
  [OpenEmu](https://openemu.org/) provide the interfaces used by their
  respective front ends.

The bundled fonts retain their own licences: Iosevka by Renzhi Li (SIL OFL
1.1), IBM Plex by IBM (SIL OFL 1.1), Roboto and Material Icons by Google
(Apache 2.0), and GNU Unifont. The associated licence files are kept beside
the font assets where supplied.

## Build

The desktop builds use SDL3. Install the platform dependencies, then build
from the relevant directory.

### macOS

```sh
brew install sdl3
make -C platforms/macos dist
```

### Linux

```sh
sudo apt install build-essential libsdl3-dev libgtk-3-dev
make -C platforms/linux
```

### OpenEmu

With a local checkout of the OpenEmu SDK:

```sh
make -C platforms/openemu SDK_ROOT=/path/to/OpenEmu-SDK
```

The OpenEmu core provides the reduced SC-3000 cartridge-computer target.

## Development tools

Desktop builds include the debugger and can be built with the embedded MCP
debugging server where supported by the platform Makefile.

## License

GearSF7000-devel is licensed under the GNU Affero General Public License v3.0. See
[LICENSE](LICENSE).
