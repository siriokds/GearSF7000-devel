// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#pragma once
#include "../../src/VideoCatalog.h"

struct VideoMenuOption
{
    std::string code, label;
    bool selected = false;
    bool applied = false;
};

// UI model shared by the menu and its tests. Empty code means machine default.
inline std::vector<VideoMenuOption> BuildVideoMenuOptions(
    const std::vector<const VideoMachineOption*>& options,
    const VideoMachineOption* defaultOption, const std::string& selection,
    const std::string& active, bool activeDefault, bool sameMachine)
{
    std::vector<VideoMenuOption> rows;
    if (!defaultOption) return rows;
    rows.push_back({{}, "Machine default (" + defaultOption->label + ")",
        selection.empty(), sameMachine && activeDefault && active == defaultOption->code});
    for (const auto* option : options)
        rows.push_back({option->code, option->label, selection == option->code,
            sameMachine && !activeDefault && active == option->code});
    return rows;
}
