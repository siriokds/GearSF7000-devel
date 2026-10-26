// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#pragma once
#include "config.h"
#include "../../src/VideoFrame.h"

// Effective values are a copy: a temporary restriction never destroys preferences.
inline config_VideoOutput ResolveVideoOutput(const config_VideoOutput& preference,
    const std::string& option, bool debug, const GC_VideoFrameDescriptor& frame)
{
    auto output = preference;
    const auto& policy = GetVideoCatalog().OutputPolicy(option, debug, preference.overscan == 4);
    output.scale = policy.Resolve("SCALE", output.scale);
    output.ratio = policy.Resolve("ASPECT_RATIO", output.ratio);
    output.overscan = policy.Resolve("OVERSCAN", output.overscan);
    output.postprocessing = policy.Resolve("POST_PROCESSING", output.postprocessing);
    if ((output.postprocessing == 2 && !frame.SupportsPostProcessing(GC_VIDEO_POSTPROCESSING_CAPABILITY_SCANLINE_CRT)) ||
        ((output.postprocessing == 1 || output.postprocessing == 3) && !frame.SupportsPostProcessing(GC_VIDEO_POSTPROCESSING_CAPABILITY_SPATIAL_SCALING)))
        output.postprocessing = 0;
    return output;
}
