# vrEmuTms9918 upstream snapshot

This directory contains a source snapshot of
[visrealm/vrEmuTms9918](https://github.com/visrealm/vrEmuTms9918).

- Revision: `dc40960`
- License: MIT; see `LICENSE`
- Imported files: `src/vrEmuTms9918.c`, `src/vrEmuTms9918.h`

The snapshot deliberately predates GearSF7000's experimental undocumented-mode
changes. It supplies the replaceable legacy renderer used by the first
Pico9918 integration milestone. GearSF7000-specific behavior belongs in the
adapter under `src/`, so updating this directory remains a source replacement
plus regression-test operation.

One local portability patch removes the redundant `<memory.h>` include. On
case-insensitive filesystems it resolves to GearSF7000's `Memory.h` before the
C library header; `<string.h>`, already included upstream, declares every
routine this source uses. No behavior is changed.

`VRAM_STORAGE_SIZE` is a second, opt-in integration seam. It defaults to the
upstream 16 KiB allocation, while `Pico9918LegacyCoreBridge.c` selects 64 KiB
of physical backing storage. `VRAM_SIZE` and `VRAM_MASK` remain 16 KiB, so the
legacy CPU port and renderer retain their original addressing behavior.

`VR_EMU_TMS9918_RENDER_LINES` likewise defaults to the original 192 lines.
The Pico adapter selects 240 so the unchanged legacy tile address generators
can draw rows 24-29 when R49.ROW30 is active. Selection and frame timing stay
in `Pico9918`; the upstream renderer is still 192 lines in its default build.

`VR_EMU_TMS9918_HOST_FIELDS` and `VR_EMU_TMS9918_EXTENDED_ROWS` are opt-in
per-instance seams for ROW30 sprite semantics. In that mode D0h is a coordinate
rather than a SAT terminator and Y wrapping starts above EFh. Both macros are
empty/inactive in an unmodified upstream build.

The adapter also widens the scanline argument from 8 to 16 bits and selects a
480-line ceiling. `VR_EMU_TMS9918_DOUBLE_ROWS` keeps that change inert by
default; the Pico adapter uses it to halve only the sprite Y coordinate while
the legacy background address generators consume all 384 or 480 logical rows.

`VR_EMU_TMS9918_TILE_COLOR` and `VR_EMU_TMS9918_SPRITE_COLOR` preserve local
four-bit colour indices by default. The Pico adapter maps them independently
into the 64-entry palette selected by R24 before returning the indexed line.

`VR_EMU_TMS9918_MAX_SCANLINE_SPRITES` and `VR_EMU_TMS9918_MAX_SPRITES` keep
the original limits by default. The Pico adapter supplies R30 and R51 so the
same renderer can process the extended sprite counts.

`VR_EMU_TMS9918_REAL_SPRITE_Y` leaves the original Y-minus-one convention in
place by default. The Pico adapter enables direct SAT Y coordinates from
R49.Y_REAL.

The tile ECM hooks default to level zero. The Pico bridge supplies R49's ECM
level, R29's pattern-plane stride and the official attribute-to-palette index
mapping; the first integration stage applies these to Graphics I tiles.

`VR_EMU_TMS9918_SPRITE_ECM`, `VR_EMU_TMS9918_SPRITE_ECM_OFFSET` and
`VR_EMU_TMS9918_SPRITE_ECM_COLOR` follow the same opt-in pattern for sprites.
Their defaults keep the upstream single-plane path unchanged. The Pico bridge
supplies R49 bits 1:0, R29 bits 7:6 and the ECM palette mapping; zero across
all enabled planes remains transparent. This seam deliberately covers planar
colour composition only: per-sprite enhanced size, flip and opaque controls
remain outside the current milestone.

`VR_EMU_TMS9918_CAPTURE_BACKGROUND` and
`VR_EMU_TMS9918_CAPTURE_SPRITE_PIXEL` are inert observation hooks. The Pico
adapter uses them to retain the pre-sprite scanline and the pixels actually
written by the sprite compositor. This supplies the generic debugger's
Background, Sprites and Dim hidden BG preview without duplicating the legacy
renderer or changing its output.
