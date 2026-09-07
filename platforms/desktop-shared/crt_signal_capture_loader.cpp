/*
 * GearSF7000 - GCRT capture loader for the GPU composite decode test
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "crt_signal_capture_loader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace
{
constexpr int kDotsPerLine = 342;
constexpr int kFrameLinesPAL = 313;

enum RecordType : std::uint8_t
{
    RecordConfigure = 0,
    RecordSync = 1,
    RecordBlank = 2,
    RecordColourBurst = 3,
    RecordData = 4
};
}  // namespace

bool CrtSignalCaptureLoader::Load(const std::string& path, std::string* error)
{
    indexTexels.clear();

    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr)
    {
        if (error) *error = "could not open " + path;
        return false;
    }
    std::vector<std::uint8_t> data;
    std::uint8_t buffer[65536];
    size_t got;
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0)
        data.insert(data.end(), buffer, buffer + got);
    fclose(file);

    if (data.size() < 5 || std::memcmp(data.data(), "GCRT", 4) != 0)
    {
        if (error) *error = "not a GCRT capture (bad magic)";
        return false;
    }

    bool isPAL = false;
    bool haveConfig = false;
    // One entry per line: the displayed dots for that line, or empty for a
    // line that carried no picture (sync/blanking).
    std::vector<std::vector<std::uint8_t>> lines;
    std::vector<int> syncDots;
    std::vector<std::uint8_t> current;
    bool inLine = false;

    size_t p = 5;
    while (p + 3 <= data.size())
    {
        const std::uint8_t type = data[p];
        const int dots = data[p + 1] | (data[p + 2] << 8);
        p += 3;
        if (type == RecordConfigure)
        {
            if (p + 2 > data.size()) break;
            isPAL = data[p] != 0;
            haveConfig = true;
            p += 2;
            continue;
        }
        if (type == RecordSync)
        {
            if (inLine) lines.push_back(current);
            current.clear();
            inLine = true;
            syncDots.push_back(dots);
        }
        else if (type == RecordData)
        {
            if (p + 2 > data.size()) break;
            const int count = data[p] | (data[p + 1] << 8);
            p += 2;
            if (p + static_cast<size_t>(count) > data.size()) break;
            if (inLine)
                current.assign(data.begin() + p, data.begin() + p + count);
            p += count;
        }
        // RecordBlank/RecordColourBurst carry no payload beyond dots.
    }
    if (inLine) lines.push_back(current);

    if (!haveConfig)
    {
        if (error) *error = "capture has no configure record";
        return false;
    }
    if (!isPAL)
    {
        if (error) *error = "NTSC capture - the composite decode shaders are PAL-only for now";
        return false;
    }

    int start = -1;
    for (size_t i = 0; i < syncDots.size() && i < lines.size(); i++)
    {
        if (syncDots[i] == kDotsPerLine) { start = static_cast<int>(i); break; }
    }
    if (start < 0 || lines.size() < static_cast<size_t>(kFrameLinesPAL))
    {
        if (error) *error = "capture too short - no full field sync found (need at least one full PAL frame after the boundary)";
        return false;
    }

    // Rotate so row 0 is the first picture line after vertical sync and
    // top blanking - same convention as bench_decode.cpp's LoadFrame.
    std::vector<std::vector<std::uint8_t>> frame;
    frame.reserve(kFrameLinesPAL);
    for (int k = 0; k < kFrameLinesPAL; k++)
        frame.push_back(lines[(start + k) % lines.size()]);
    std::rotate(frame.begin(), frame.begin() + 3 + 13, frame.end());
    frame.resize(kPictureLines);

    indexTexels.assign(static_cast<size_t>(kPictureDots) * kPictureLines, 0);
    for (int row = 0; row < kPictureLines; row++)
    {
        const std::vector<std::uint8_t>& idx = frame[row];
        for (int dot = 0; dot < kPictureDots; dot++)
        {
            const std::uint8_t value = (dot < static_cast<int>(idx.size()))
                ? (idx[dot] & 0x0F) : 1;  // 1 = black, matches the encoder's own substitution
            // 15 discrete levels map exactly onto 0..255 (17 apart), so
            // decoding with round(sampled * 15.0) in the shader is exact.
            indexTexels[static_cast<size_t>(row) * kPictureDots + dot] =
                static_cast<std::uint8_t>(value * 17);
        }
    }
    return true;
}
