GearSF7000 platform targets
===========================

Public project: https://github.com/siriokds/GearSF7000-devel

Desktop: Windows (Visual Studio) and Linux. Both build the same desktop
application from ../desktop-shared; see the root README for instructions.

Web: the libretro core (libretro/) and web/player.cpp, compiled with
Emscripten by web/build-player.sh into a generic page (web/generic/) that runs
a cartridge chosen from the disk, drawn on a 2D canvas.

Runtime resources include SDL3, the controller database, fonts, keyboard
image, debugger port descriptions and optional firmware symbols. C-BIOS is
embedded in the desktop build and does not include MSX BASIC.

GearSF7000 is a project by Saverio Russo. It uses code from other projects,
credited in the root README. Preserve upstream copyright notices and
third-party licenses when distributing.
The emulator is distributed under AGPL-3.0-or-later; the code from other
projects keeps its own licence, and each file's header says which applies. See the root LICENSE, LICENSE.GPL-3.0
and individual dependency licenses for their respective terms.
