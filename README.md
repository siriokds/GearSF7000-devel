# GearSF7000-devel

GearSF7000 is a cross-platform emulator and development environment for the
Sega SF-7000, SC-3000, SR-1000 and SP-400, written in C++.

This repository contains the development sources. It follows the layout used
by the other Gear emulators: the emulator core is in `src`, while desktop,
libretro and OpenEmu front ends live in `platforms`.

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

GearSF7000 is licensed under the GNU General Public License v3.0. See
[LICENSE](LICENSE).
