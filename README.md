# GearSF7000-devel

[![Workflow status](https://img.shields.io/github/actions/workflow/status/siriokds/GearSF7000-devel/build-release.yml)](https://github.com/siriokds/GearSF7000-devel/actions/workflows/build-release.yml)
[![Release](https://img.shields.io/github/v/tag/siriokds/GearSF7000-devel?label=version)](https://github.com/siriokds/GearSF7000-devel/releases)
[![Activity](https://img.shields.io/github/commit-activity/t/siriokds/GearSF7000-devel)](https://github.com/siriokds/GearSF7000-devel/commits/main)

GearSF7000 emulates two base systems: the Sega SG-1000 console and the Sega
SC-3000 computer. The SC-3000 configuration can be expanded with the Sega
SF-7000 Super Control Station and the Sega SR-1000 Tape Recorder. SF-7000 is
an expansion peripheral, not a standalone computer: it connects through its
dedicated IPL cartridge, which occupies the cartridge slot.

The emulator combines accurate processor and video timing with disk and
cassette support, an advanced frame recorder and a full development
environment built into the desktop application. Support for the Sega SP-400
Printer/Plotter is planned.

Run cartridge software on the base machines, connect and boot the SF-7000
through its dedicated IPL cartridge, work with SegaDOS disks, load Sega BASIC
programs or follow a single VDP access down to the exact raster dot and VRAM
slot. GearSF7000 is designed both for enjoying the original machines and for
understanding precisely what they are doing.

## Highlights

- **Two base systems:** SG-1000 console and SC-3000 computer, each with its
  own memory map and hardware configuration.
- **SC-3000 peripherals:** Sega SK-1100 keyboard handling, Sega SF-7000 Super
  Control Station and Sega SR-1000 Tape Recorder.
- **Timing you can inspect:** partial Z80 machine cycles, native PAL/NTSC
  raster timing and a slot-accurate TMS9918/TMS9929 VRAM scheduler.
- **All VDP mode combinations:** Graphics I, Graphics II, Text, Multicolor and
  the four undocumented combinations produced by the hardware mode bits.
- **Real media workflows:** cartridge and ZIP images, multiple floppy formats,
  SegaDOS filesystem inspection, WAV tapes and Sega BASIC program transfer.
- **Machine audio beyond the PSG:** SN76489, optional AY-3-8910/YM2149,
  cassette-speaker monitoring, floppy activity audio and WAV/VGM recording.
- **Time-travel development:** save states, a frame recorder, rewind,
  branching from earlier frames and analysis of memory changes over time.
- **Deep integrated debugging:** CPU, disassembly, memory, symbols, VDP,
  sprites, raster, audio, cassette, disk, FDC765, serial and event tools.
- **External tooling:** an optional MCP server with 132 commands and headless
  operation over standard input/output or local HTTP.

## Downloads

Current packages are published on the
[Releases page](https://github.com/siriokds/GearSF7000-devel/releases). Windows is
available as a ready-to-run desktop package. macOS, Linux and BSD can be built
from source using the instructions below.

## Hardware coverage

| Area | Implemented hardware and behaviour |
| --- | --- |
| CPU | Zilog Z80 with documented and undocumented behaviour, register R, MEMPTR, interrupt lines, reset handling and instruction-level inspection. |
| Video | TMS9918/TMS9929 VDP with raster timing, VRAM arbitration, sprite evaluation, status flags and its internal 171-slot scanline schedule. |
| Sound | SN76489 PSG, optional AY-3-8910 / YM2149 expansion, resampling, VGM/WAV capture and machine-speaker paths. |
| Base systems | SG-1000 console mapping and SC-3000 computer memory map, PPI, keyboard, cartridge handling and Sega BASIC workflows. |
| Sega SF-7000 Super Control Station | Expansion with dedicated IPL cartridge, FDC765 floppy subsystem, drive control, disk timing, SegaDOS media and Intel 8251A serial interface. |
| Sega SR-1000 Tape Recorder | Cassette motor, tape transport, tape speaker and WAV/Sega BASIC tape handling. |

## Timing and fidelity

GearSF7000 models the relationship between the processor and the devices;
the machine does not advance as a frame-only approximation.

- The Z80 executes through partial machine cycles and exposes the bus
  operations needed by memory, I/O, interrupts and wait-state-sensitive
  peripherals.
- The VDP follows the native 342-dot raster, with 262 lines for NTSC and 313
  for PAL. Its VRAM activity uses the documented 171-slot line schedule to
  distinguish CPU accesses, refresh, background fetches and sprite fetches.
- Every one of the 171 VRAM slots is classified for the active calendar. The
  Graphics calendar records name, colour and pattern fetches together with
  sprite scan/selection/fetch activity; Text uses its 40-column name/pattern
  calendar; Multicolor releases the unused colour-table slots to the CPU.
  Blanking uses the refresh calendar, so CPU-port availability follows the
  beam and display state rather than a fixed delay.
- VRAM transactions run below raster-dot resolution on a 684-phase line: 171
  slots of four phases each. CPU requests enter a single hardware-style port
  latch, become eligible after the internal delay and can issue only during a
  CPU-owned DRAM slot. Writes commit and reads latch at distinct phases inside
  the transaction; a new port access can replace a queued request but cannot
  change one already issued. The RAS alignment is kept as a configurable
  hardware-measurement parameter rather than buried in the renderer.
- Raster line, dot and slot metadata are retained with diagnostic events, so
  timing faults can be inspected at the point where they occur.
- The FDC765, disk media and drive state are emulated as hardware components,
  including the sound-producing floppy path and its timing-sensitive activity.
- Tape data, motor state and speaker output advance with the machine clock.
- Save states preserve processor execution, memory, VDP, audio, peripherals
  and clock-domain state for consistent continuation after restoration.

### TMS9918/TMS9929 video modes

The renderer enables and handles all eight combinations of the three VDP mode
bits, including combinations that are not part of the standard programming
modes. A disabled entry in the sprite column describes a characteristic of
that VDP mode, not missing emulator support; the Text modes disable sprites by
design.

| Mode | Rendering behaviour | Sprite plane |
| --- | --- | --- |
| 0 | Graphics I | Enabled |
| 1 | Text, 40 columns of 6-pixel characters | Disabled |
| 2 | Graphics II with screen-region pattern and colour banking | Enabled |
| 3 | Undocumented Text with Graphics II addressing and banking | Disabled |
| 4 | Multicolor | Enabled |
| 5 | Undocumented fixed foreground/backdrop bar pattern | Disabled |
| 6 | Undocumented Multicolor with Graphics II addressing and banking | Enabled on TMS9918 |
| 7 | Undocumented fixed foreground/backdrop bar pattern | Disabled |

Mode changes, register 7 colour changes and display blanking are tracked at
raster position. Mid-line changes therefore affect the part of the line that
the beam has not produced yet. Sprite selection, fifth-sprite overflow and
collision state follow the corresponding scanline pipeline.

### Clock domains and device synchronisation

The emulated devices retain their physical oscillator domains:

| Domain | NTSC | PAL |
| --- | ---: | ---: |
| Z80, PSG and CPU-domain devices | 3,579,545 Hz | 3,580,000 Hz |
| TMS9918/TMS9929 master clock | 10,738,635 Hz | 10,738,635 Hz |
| SF-7000 FDC clock | 8,000,000 Hz | 8,000,000 Hz |
| Native frame rate | 59.9226 Hz | 50.1591 Hz |

Objective Z80 half-cycles are converted independently into whole CPU
T-states, VDP master clocks and FDC clocks. The conversion uses exact rational
accumulators and retains every fractional remainder; those remainders are also
stored in save states. PAL video can therefore remain asynchronous to the CPU
without accumulating rounding drift, while PSG, cassette, serial, keyboard
events, VDP and disk controller all advance from their correct domains.

Frame pacing keeps machine time separate from presentation time. **Accurate**
mode runs from the physical clocks above: on a 60 Hz monitor an NTSC frame is
repeated roughly once every 775 presentations so the machine remains at
59.9226 Hz. **Gaming + VSync** can instead lock a machine frame to each 60 Hz
display refresh (or each pair at 120 Hz). This runs the complete machine about
0.13% faster while preserving every internal clock ratio and the exact number
of CPU, VDP and FDC clocks inside a frame. Displays such as 75 or 144 Hz that
are not close to an integer multiple use the time accumulator rather than an
incorrect rounded divisor.

### Audio quality and synchronisation

Audio is generated from emulated clock events and mixed from separate PSG,
AY, floppy-drive and cassette-speaker paths.

- SN76489 and AY/YM2149 writes retain their emulated timing in a coherent
  Blip_Buffer time domain. The AY divider and output level are calibrated
  independently instead of being inferred from incompatible amplitude tables.
- PSG output is attenuated to represent the SC-3000 A/V output network;
  mechanical drive samples and cassette monitoring retain separate gain
  staging.
- A smooth limiter begins near full scale, avoiding the discontinuity and
  harsh distortion of integer clipping when several sources peak together.
- Drive and cassette producers retain fractional per-frame sample remainders
  in carry queues, so samples that cross a PSG frame boundary are consumed in
  the following frame rather than discarded.
- The optional AY expansion is disconnected cleanly when absent, including
  its resting DC component, preventing an artificial startup click.
- Drift between the host audio device and emulated crystal is corrected in
  the resampler instead of by changing machine speed. The scheduler filters
  the device's block-drain ripple, targets a half-full queue and reports fill,
  underruns, dropped samples and the applied correction.
- WAV capture records the mixed output. VGM capture records register writes
  and declares the machine's nominal regional crystal, remaining independent
  of host-side resampler correction.
- Per-channel PSG and AY waveforms are captured only while their diagnostic
  panels request them. Channel filters, peak meters and producer queue counters
  make the whole mix inspectable.

### SR-1000 tape-head speaker

The SR-1000 monitor is a clocked speaker model of its own, separate from the
logic level delivered to the PPI and from the computer's PSG.

- WAV tapes feed the monitor from the virtual tape head through the original
  cycle-timed sample path.
- Synthesised bit tapes use a phase-continuous oscillator driven by timestamped
  FSK frequency changes. PPI edge chatter cannot restart its phase or fill the
  queue with duplicate events.
- Event timestamps are measured in SC-3000 clock cycles and consumed against
  the master audio cadence. Events closer than a valid cassette symbol can be
  coalesced without altering the tape data seen by the machine.
- Its cycles-to-samples conversion matches Blip_Buffer's fixed-point rate,
  avoiding a separate cassette clock that would drift ahead and produce
  frame-rate modulation in the final mix.
- Output is stereo-aligned with the PSG, intentionally quieter than the
  computer audio and carried across frame boundaries without losing remainder
  samples.
- A live frequency meter checks the reproduced pilot and data tones. Invalid
  inputs, queue pressure, coalescing and timestamp rebasing are counted so a
  malformed image or debugger pause fails safely without corrupting PPI tape
  timing.

## Emulated systems, peripherals and media

The base system is selected independently from its peripherals. The cartridge
slot is modelled as a physical resource: a normal software cartridge and the
SF-7000 IPL cartridge cannot occupy it together.

- **SG-1000:** base console with its cartridge-oriented memory mapping.
- **SC-3000:** base computer with keyboard-matrix input, BASIC workflows and
  cartridge software.
- **Sega SF-7000 Super Control Station:** optional SC-3000 disk expansion,
  connected through its IPL cartridge, with boot, drive and disk controls.
- **Sega SR-1000 Tape Recorder:** optional SC-3000 cassette peripheral with
  transport controls, tape files and speaker monitoring.
- **Sega SP-400 Printer/Plotter:** planned peripheral; it is not presented as
  implemented.

Supported media include:

- **Disk images** in `.sf7`, `.dsk`, `.hfe` and `.ds7` formats, with mount,
  eject and write-protection control.
- **Cartridge images** and ZIP-compressed media, with a database of known
  mappings.
- **Modern controllers** through the bundled
  [SDL_GameControllerDB](https://github.com/gabomdq/SDL_GameControllerDB).

## Desktop emulator

The standalone application is intended both as a conventional emulator and as
a precise hardware workbench.

- Native desktop targets for Windows, macOS, Linux and BSD.
- SDL3 desktop runtime and SDL GPU renderer.
- Dear ImGui interface with docking and multi-viewport support in development
  builds.
- Native file dialogs: AppKit on macOS and GTK3 on Linux.
- Drag and drop, recent-media lists, command-line media loading and optional
  portable mode.
- Save-state slots, state files, screenshots and fast-forward controls.
- PAL/NTSC selection, overscan, aspect-ratio options and renderer-source
  inspection.
- User-selectable post-processing with Bilinear, Advanced Scaling and CRT
  (Lottes). CRT controls include scanline shape, phosphor-mask type and
  intensity, gamma, sharpness and dark-edge reconstruction.
- Embedded shader bytecode for Vulkan/SPIR-V, macOS Metal and Windows
  Direct3D 12/DXIL; ordinary builds do not require shader-compilation tools.
- Additional development passes for captured composite/S-Video decoding and
  consumer-TV experiments. These remain diagnostic tools and are not yet the
  complete television presentation planned for the emulator interface.

## Sega BASIC tools

GearSF7000 includes helpers that make software development and recovery of
Sega BASIC programs practical without bypassing the emulated machine.

- **BASIC Typer** enters text through the emulated SK-1100 keyboard matrix,
  with queue status and cancellation.
- BASIC programs can be loaded from and saved to `.bas` files.
- The program-pointer block can be selected explicitly or located by scanning
  the running machine, supporting the different layouts used by cartridge and
  Disk BASIC.
- Tape conversion supports BASIC data through the same cassette path used by
  the emulator; the motor, transport and speaker remain part of the machine.
- Disk-oriented workflows can inspect SegaDOS files, directories, allocation
  data and file types through Disc Explorer.

## Debugger and inspection tools

The complete desktop build provides integrated, dockable tools that operate on
the same live machine state as the emulator.

### Execution and CPU

- Z80 register, flag and clock views; reset and interrupt control.
- Pause, continue, instruction step, step over, step out, line step, frame
  step and run to cursor.
- Execution, read, write and range breakpoints, including interrupt
  breakpoints.
- Disassembler with live PC following, opcode bytes, symbols, segments,
  banking, navigation history, code coverage and export.
- Call-stack inspection, symbol loading and lookup, automatic labels and
  disassembly bookmarks.

### Memory and program data

- Memory editor for RAM, ROM and VRAM with copy/paste, import/export, range
  selection and value filling.
- Bookmarks, watches, conditional watches and freeze controls.
- Byte-pattern search, advanced memory search and capture of a search base.
- ROM Inspector for cartridge identity, mapping and loaded-media information.
- Memory import/export and direct inspection of the active machine map.

### Video and raster

- VDP register and status windows with decoded values.
- VDP Viewer for tiles, name tables, patterns, colours, sprites and
  backgrounds.
- Full 342-dot raster view, including border, blanking, synchronisation and
  colour-burst regions where applicable.
- Sprite list, per-scanline selection, fifth-sprite overflow, collision state
  and sprite-pipeline inspection.
- Raster, dot, slot and VRAM-port activity history for timing diagnosis.
- Screenshot, raw framebuffer and CRT-signal capture tools.

### Audio, cassette, disk and SF-7000

- SN76489 PSG viewer and optional AY-3-8910 / YM2149 viewer, with register,
  channel and mixer inspection.
- WAV and VGM recording.
- Cassette transport, tape speed and tape-speaker controls.
- SF-7000 panel for IPL, drive, FDC765 and serial-interface status.
- Disc Explorer for SegaDOS media, directories, allocation data and file
  inspection.
- Intel 8251A serial emulation with timing, status and configurable host
  backends.

### Recorder, traces and events

- Recorder window for a bounded sequence of complete machine snapshots. It is
  enabled by default with 60 seconds of history and can be configured from 1
  to 600 seconds.
- The default detail is one snapshot for every native machine frame. External
  control can select a larger frame stride when longer, coarser history is
  more useful.
- Every snapshot contains media identity, region, mapper and frame serial;
  memory; the complete Z80 execution state; clock counters and rational
  remainders; VDP; audio; SK-1100 keyboard; PPI; and, when active, SF-7000
  controller and peripheral state.
- Frames are stored in groups of 60. The first is a compressed full keyframe;
  following frames are compressed XOR deltas, preserving exact state while
  avoiding the cost of duplicating mostly unchanged RAM and VRAM.
- The default memory ceiling is 512 MiB. The recorder reports raw state size,
  actual memory use, compression ratio, projected capacity, capture cost and
  dropped or failed snapshots; it refuses a requested duration that cannot
  fit instead of silently shortening it.
- Frame-accurate rewind, transport controls, resume from a previous point and
  branching history. Seeking pauses the machine; resuming from the past
  discards the superseded future so the timeline remains coherent.
- Memory-range analysis between recorded points, including a baseline and
  changed spans.
- Trace log with CPU, memory, I/O, VDP, FDC, tape, audio and device-state
  events.
- Event rules with address ranges, value masks, transitions, hit counts and
  one-shot pause conditions.
- Scheduler diagnostics for frame pacing, display synchronisation and audio
  flow.

## MCP debugging server

The desktop development build can include an embedded MCP server for external
debugging and tooling. It supports standard-input/output and local HTTP
transports, headless operation and an optional access token for the HTTP
endpoint.

Headless mode runs the same emulation core without creating a window or
opening an audio device. It requires an MCP transport so the machine can be
controlled and inspected while running unattended.

The current server exposes **132 commands** across execution, breakpoints,
memory, CPU, disassembly, symbols, video, audio, SF-7000 hardware, media,
capture, save states, recorder, input and trace categories. These commands use
the same debugger services as the desktop windows; they do not maintain a
separate or approximate machine model.

Available operations include reading or writing memory, setting device-level
rules, inspecting VDP slots and sprite pipelines, mounting media, operating
the recorder, extracting screenshots and querying FDC, PSG, serial-port or
CPU state.

### Why the MCP interface is useful

The server turns the emulator into a machine-readable hardware laboratory. A
debugging client can pause at an exact event, obtain an atomic view of CPU,
memory and device state, inspect the raster position that produced it and then
continue from the same point. This avoids conclusions assembled from values
captured at different moments.

Event rules make it possible to stop on causes instead of symptoms: a
particular I/O access, FDC transition, VDP register write, VRAM address, audio
event or masked value change. The recorder can then move backward through the
preceding frames, compare memory ranges and resume from an earlier state to
test a different path.

Headless execution makes the same process reproducible without desktop input.
A client can load media, drive the keyboard or controllers, execute a precise
amount of work, collect traces, inspect screenshots and repeat the sequence.
That combination shortens the loop from observation to hypothesis and from
hypothesis to a verifiable result.

Some commands are especially useful when diagnosing difficult faults:

| Commands | What they make possible |
| --- | --- |
| `get_atomic_snapshot` | Capture CPU, selected memory and device state at one coherent instant. |
| `add_debug_rule`, `add_device_debug_rule`, `get_debug_events` | Stop on the hardware transition that caused a fault and retrieve its ordered history. |
| `configure_rewind`, `rewind_seek`, `analyze_rewind_range` | Travel back before a failure and identify exactly which memory spans changed. |
| `get_slot_history`, `get_vdp_status`, `get_sprite_pipeline` | Explain a video fault at the raster-dot and VRAM-slot level. |
| `read_framebuffer`, `get_screenshot`, `get_full_raster_debug_screenshot` | Compare the visible result, palette-index output and complete raster. |
| `memory_search_capture`, `memory_search`, `memory_find_bytes_advanced` | Discover unknown variables and data structures by observing how memory evolves. |
| `keyboard_text`, `controller_macro`, `basic_typer_set_text` | Reproduce long input sequences exactly without manual timing variations. |
| `mount_disk`, `load_tape`, `load_rom`, `start_sf7000` | Construct and restart a complete media scenario under external control. |
| `debug_step_into`, `debug_step_over`, `debug_step_out`, `debug_run_to_cursor` | Move from machine-level symptoms to the responsible instruction sequence. |

## Build configurations

The codebase supports feature-oriented builds. The complete desktop target
enables the development environment; reduced products can remove the debugger,
recorder, MCP server and optional hardware without duplicating the core.

| Build switch | Purpose |
| --- | --- |
| `GEARSF7000_ENABLE_DEBUG_TOOLS` | Desktop debugger, inspection windows and diagnostic UI. |
| `GEARSF7000_ENABLE_RECORDER` | Frame recorder and rewind support. |
| `GEARSF7000_ENABLE_MCP` | Embedded MCP debugging server. |
| `GEARSF7000_ENABLE_AY` | AY-3-8910 / YM2149 expansion audio. |
| `GEARSF7000_ENABLE_SF7000` | SF-7000 hardware. |
| `GEARSF7000_ENABLE_SR1000` | SR-1000 cassette recorder. |
| `GEARSF7000_ENABLE_SP400` | SP-400 build hook for future support. |
| `GEARSF7000_PRODUCT_SC3000` | Reduced cartridge-computer target. |

## Building from source

Desktop front ends use C++20 and SDL3. Platform scripts and makefiles keep
intermediate objects in separate profile directories, so full and reduced
builds do not reuse incompatible objects.

### Windows

1. Install Visual Studio Community with C++ desktop development tools.
2. Put the SDL3 VC development package in
   `platforms/windows/dependencies/SDL3/`.
3. Open `platforms/windows/GearSF7000.sln` and build the desired target.

### macOS

Install SDL3, then build the local application bundle:

```sh
brew install sdl3
platforms/macos/build.sh
```

The resulting `platforms/macos/GearSF7000.app` includes its keyboard image
and font resources. Add `--no-mcp` to build without the embedded debugging
server.

### Linux

On Ubuntu or Debian:

```sh
sudo apt install build-essential libsdl3-dev libgtk-3-dev
make -C platforms/linux
```

The reduced SC-3000 cartridge-computer target is available with:

```sh
make -C platforms/linux computer
```

### BSD

Install SDL3, GTK3, a C++ compiler and GNU Make through the package manager,
then build from `platforms/bsd` with `gmake`.

### Libretro and OpenEmu

Dedicated front ends are present under `platforms/libretro` and
`platforms/openemu`. The OpenEmu core is a reduced SC-3000/SG-1000 target;
see [its platform notes](platforms/openemu/README.md) for its media support
and OpenEmu SDK build instructions.

## Technical foundations and credits

GearSF7000 is based on a version of
[GearColeco](https://github.com/drhelius/Gearcoleco) by
[Ignacio Sánchez Gines (drhelius)](https://github.com/drhelius), adapted for
the Sega SG-1000 and SC-3000 systems and their supported peripherals.

The Z80 processor integration uses an imported subset of
[Clock Signal (CLK)](https://github.com/TomHarte/CLK) by
[Tom Harte](https://github.com/TomHarte), adapted to this project's timing and
machine requirements. The imported revision and its MIT licence are recorded
in [`third_party/clk/README-GearSF7000.md`](third_party/clk/README-GearSF7000.md).

The FDC765 and SF-7000 modules originate from
[blueMSX](http://www.bluemsx.com/) by Daniel Vik and Tomas Karlsson and have
been extensively modified for this project.

GearSF7000 also incorporates or builds against the following projects. Their
licence files and notices remain applicable to the corresponding components.

| Project | Author or maintainers | Use in GearSF7000 |
| --- | --- | --- |
| [GearColeco](https://github.com/drhelius/Gearcoleco) | Ignacio Sánchez Gines (drhelius) | Original project foundation and Gear desktop architecture. |
| [Clock Signal (CLK)](https://github.com/TomHarte/CLK) | Tom Harte | Imported Z80 subset, adapted to the machine and stack requirements (MIT). |
| [blueMSX](http://www.bluemsx.com/) | Daniel Vik and Tomas Karlsson | Original basis of the extensively modified FDC765 and SF-7000 modules. |
| [Game Music Emu](https://bitbucket.org/mpyne/game-music-emu/) and [Blip_Buffer](https://www.slack.net/~ant/) | Shay Green | Sound generation and resampling components. |
| [SDL3](https://github.com/libsdl-org/SDL) | SDL project contributors | Cross-platform desktop runtime, input, audio and GPU interface. |
| [Dear ImGui](https://github.com/ocornut/imgui) | Omar Cornut and contributors | Desktop user interface and debugger windows (MIT). |
| [ImPlot](https://github.com/epezent/implot) | Evan Pezent, Breno Cunha Queiroz and contributors | Diagnostic plots (MIT). |
| [Native File Dialog Extended](https://github.com/btzy/nativefiledialog-extended) | Bernard Teo, Michael Labbe and contributors | Native desktop file dialogs. |
| [JSON for Modern C++](https://github.com/nlohmann/json) | Niels Lohmann and contributors | JSON and MCP message handling (MIT). |
| [mINI](https://github.com/pulzed/mINI) | Danijel Durakovic | INI configuration files (MIT). |
| [miniz](https://github.com/richgel999/miniz) | Rich Geldreich and contributors | ZIP and deflate support. |
| [dr_libs](https://github.com/mackron/dr_libs) | David Reid | MP3 decoding through `dr_mp3`. |
| [stb](https://github.com/nothings/stb) | Sean Barrett and contributors | Image loading and writing. |
| [SDL_GameControllerDB](https://github.com/gabomdq/SDL_GameControllerDB) | Community contributors | Bundled controller mapping database. |
| [Libretro](https://www.libretro.com/) | Libretro contributors | Libretro front-end interface. |
| [OpenEmu](https://openemu.org/) | OpenEmu contributors | Native macOS front-end interface. |

Bundled fonts retain their own licences and authorship: Iosevka by Renzhi Li,
IBM Plex by IBM, Roboto and Material Icons by Google, and GNU Unifont. The
associated notices are distributed with the font assets where supplied.

## Contributing

Bug reports, compatibility reports and feature proposals are welcome through
the [issue tracker](https://github.com/siriokds/GearSF7000-devel/issues). Please
include the machine configuration, the media used, the observed result and a
short reproducible sequence when reporting a hardware issue.

## License

GearSF7000-devel is licensed under the GNU Affero General Public License v3.0.
See [LICENSE](LICENSE) for the complete terms.
