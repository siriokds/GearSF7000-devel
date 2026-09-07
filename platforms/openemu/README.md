# GearSC3000 for OpenEmu

This target builds a native, SDL-free OpenEmu core plugin with the same
reduced feature profile as the `computer` and libretro targets.

## Requirements

- macOS 11 or later
- OpenEmu installed in `/Applications`
- OpenEmu-SDK checked out next to the `siriokds` directory (the default in
  this workspace), or its path passed as `SDK_ROOT=/path/to/OpenEmu-SDK`

Build and install with:

```sh
make -C platforms/openemu
make -C platforms/openemu install
```

Create the distributable ZIP, SHA-256 file and OpenEmu appcasts with:

```sh
make -C platforms/openemu package VERSION=1.0.0
```

The GitHub workflow publishes these files when a tag named
`openemu-vVERSION` is pushed, for example `openemu-v1.0.0`. The appcast has a
stable `releases/latest/download` URL, while its enclosure points to the exact
tagged release. `gearsc3000.xml` contains the same entry under the filename
required by the OpenEmu-Silicon channel.

The OpenEmu-Silicon fork installed on the development machine forcibly
rewrites third-party core feeds to its own `Appcasts/<core>.xml` namespace.
Automatic updates in that fork therefore require a corresponding contribution
to its repository. See `openemu-silicon/` for the exact feed name, `oecores.xml`
entry and signing hand-off. Changing the core name would only change the name
of the missing feed and is not a solution.

The standalone appcast deliberately carries no Sparkle EdDSA signature. A
SHA-256 file is published beside the ZIP. When accepted into OpenEmu-Silicon,
the maintainer must sign the appcast entry using that project's private
Sparkle key.

OpenEmu's official community cores are ad-hoc signed as well. The build follows
that compatible default. If a Developer ID Application identity is available,
pass `CODE_SIGN_IDENTITY="Developer ID Application: ..."`; the machine used
for this integration currently has no such identity installed.

The install target opens the generated `.oecoreplugin`; OpenEmu then copies
it into its per-user Cores directory. Select GearSC3000 as the core for the
SG-1000 system if OpenEmu does not select it automatically.

While a game is running, OpenEmu's display-mode menu offers `Internal
256x192` (the default) and `Borders`. The latter asks the emulation engine for
the complete 272-pixel-wide picture, including the generated border: 272x224
for NTSC and 272x256 for PAL. OpenEmu saves the selection in its core display
preferences.

## OpenEmu limitation

Stock OpenEmu only accepts its built-in system plugins. Its built-in SG-1000
plugin recognizes `.sg` files and exposes joystick controls, but does not
recognize `.sc` or forward the host keyboard. Therefore this distributable
plugin runs `.sg` cartridges with both joysticks and exact PAL/NTSC timing,
but `.sc` import and the SK-1100 keyboard would require a change to OpenEmu
itself. Renaming `.sc` to `.sg` does not restore keyboard input.

ColecoVision `.col` and `.cv` content is intentionally rejected.
