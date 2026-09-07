# dr_libs — single-header audio decoder

Vendored, compiled directly into GearSF7000: no external library, no
`.dylib`, nothing to install on any platform (macOS, Linux, BSD, Windows).

| File | Version | Origin | License |
|---|---|---|---|
| `dr_mp3.h` | v0.7.4 | <https://github.com/mackron/dr_libs> | public domain (Unlicense) **or** MIT-0, either |

`DR_MP3_IMPLEMENTATION` is defined in exactly one translation unit,
`src/SegaBasicWavTape.cpp` — the only file that includes this header. That
file already owns the WAV tape decode path (`SegaBasicWavTape::LoadTape`);
`SegaBasicWavTape::LoadMp3` decodes an MP3 into the same in-memory
`int16_t*` PCM buffer / `WAVUtils::WAVStruct` shape, so every existing
signal-decode routine downstream (`Tick`, `GetSignal`, `GetSample`, ...)
works unmodified regardless of which format loaded the tape.

Decoding always downmixes to mono before storing, matching the assumption
the rest of `SegaBasicWavTape` already makes about `audioData` for real
cassette recordings (see the WAV path's own buffer sizing, which is not
multiplied by channel count).

## Updating

Replace the file and update the version above. Do not patch it locally —
if different behavior is ever needed, do it in the wrapper
(`SegaBasicWavTape::LoadMp3`), so the header stays a drop-in replacement.
