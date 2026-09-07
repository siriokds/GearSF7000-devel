# sf7fs — Sega Disk BASIC filesystem

Directory and FAT logic for SF-7000 `.sf7` disc images: list, read, extract,
insert and delete files. Compiled into the emulator.

Derived from the `sfdisc` command-line tool
(<https://github.com/siriokds/sfdisc>) — same author, same licence
(GPLv3), so this is a fork rather than a third-party import. It started as a
verbatim copy of `SF7Disc.cpp`/`.h` at commit `381cc32`, then diverged: see
below.

## What this is not

It does **not** read the disc mounted in the emulated drive. It opens a
`.sf7` file from the host filesystem, on its own, so a disc can be browsed
while the emulator has a different one inserted. `Disk.cpp` and its
`diskReadSector()` / `diskGetTrack()` are deliberately not involved.

Only flat `.sf7` is supported. `.ds7`, EDSK and HFE are not, by choice.

## How it differs from sfdisc

The command-line tool streamed the image with `std::fstream` and printed its
own errors. Neither suits a library:

- **The image lives in memory.** An SF-7000 disc is 163840 bytes, so there is
  nothing to gain from streaming, and a plain buffer makes every operation
  testable. `SF7Image::load()` and `save()` are the only host I/O; everything
  else works on the buffer. Callers save when they choose, and `isDirty()`
  says whether they need to.
- **Nothing prints.** The fourteen `std::cerr` / `std::cout` calls became
  `FATResult` values. `sf7FATErrorText()` turns one into a message.
- **The path-based wrappers are gone**, along with their duplicated bodies —
  `sf7ReadFAT` used to repeat `sf7ReadFATStream` line for line.
- **`SF7Image::readHeader()` and `discLabel()` are new.** The disc label was
  parsed inline inside the tool's print routine and could not be reused.
- **`sf7CountFreeClusters()` is new**, split out of the tool's free-space
  report.
- **`importFile()` refuses to overwrite**, returning
  `ERROR_FILE_ALREADY_EXISTS`; deciding what to do about that belongs to the
  caller. It also zeroes the unused tail of the last cluster, so a new file
  cannot expose bytes left by a deleted one.
- **`deleteFile()` validates the whole cluster chain before freeing any of
  it**, so a corrupt chain leaves the image untouched instead of half-freed.

Two bugs fixed upstream in sfdisc first, and carried here: the last cluster's
sector count was derived from a counter that had already been decremented, so
every imported file came back truncated on export; and the end-of-chain test
masked with `0xC0`, which also matches the `$FE` and `$FF` markers.

## An analyser, not a filter

`load()` takes any image of the right size and the directory reader never
hides anything. Entries filled with `$FE` are reserved padding, and they are
listed like every other entry -- `sfdisc` lists them too, and the point of the
tool is the real content of the disc rather than what Disk BASIC would choose
to show. Kamikaze is the clearest case: its whole FAT is `$FE`, so nothing is
allocatable, and its directory holds a French notice ("Pour charger KAMIKAZE
ecrire BOOT") that Disk BASIC prints when you type FILES.

`validate()` is a verdict the caller can display, never a gate. It answers
whether the image carries a Disk BASIC filesystem, by requiring legal FAT
values and printable 8.3 names in the occupied entries. Reading a disc that
fails it is allowed and often the reason to open it -- CP/M and SegaDOS keep
Z80 code where the FAT belongs, and seeing that is the answer.

## Verifying

The library's directory listing, file sizes, disc label and free-cluster
count were checked against `sfdisc`'s own output on `scdos_disk.sf7`: all
fifteen entries, all fifteen sizes, and the 61 free clusters agree. Import
and export round-trip at 1, 255, 256, 300, 1023, 1024, 1500, 2048, 2500 and
5000 bytes.

Read-only sweep over 51 real images from the SC-3000 disc collection and the
SCDOS-NATIVE working set: 499 files, 2751744 bytes, every cluster chain walked
and every file re-exported at its declared size. The twelve images that are
not Disk BASIC volumes are refused at load with a message rather than parsed
into noise.

## Updating

sfdisc and this copy have diverged and are not interchangeable any more. A
fix that belongs to the shared filesystem logic — the FAT walk, the 8.3 name
handling, the on-disc structures — is worth applying to both.
