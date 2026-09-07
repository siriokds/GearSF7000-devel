/*
 * GearSF7000 - GCRT capture loader for the GPU composite decode test
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CRT_SIGNAL_CAPTURE_LOADER_H
#define CRT_SIGNAL_CAPTURE_LOADER_H

#include <cstdint>
#include <string>
#include <vector>

// Parses a file written by CrtSignalDumpSink (crt_signal_dump.h/.cpp) into
// the flat per-dot palette-index buffer the composite decode shaders
// expect - one entry per (row, dot), 284 dots wide, 294 picture rows tall,
// matching kPictureDots/kPictureLines hardcoded into composite_chroma.frag
// and composite_luma.frag. This is the same picture geometry
// tools/crt/bench_decode.cpp reads, so a capture that works with the
// offline CPU/Python decoders works here unchanged.
//
// PAL only for now, matching bench_decode.cpp/decode_crt.py: the
// composite decode shaders hardcode PAL's 294-line visible area, so an
// NTSC capture is rejected outright rather than silently decoded wrong.
class CrtSignalCaptureLoader
{
public:
    static constexpr int kPictureDots = 284;
    static constexpr int kPictureLines = 294;

    // Values are index/15.0 in [0,1], ready to upload as an R8_UNORM
    // texture - 16 = kPictureDots * kPictureLines is not a coincidence,
    // it is exactly that many entries, row-major.
    std::vector<std::uint8_t> indexTexels;

    // Parses the capture and fills indexTexels. Returns false (with
    // indexTexels left empty) on a malformed file, an NTSC capture, or a
    // capture too short to contain a full field.
    bool Load(const std::string& path, std::string* error);
};

#endif // CRT_SIGNAL_CAPTURE_LOADER_H
