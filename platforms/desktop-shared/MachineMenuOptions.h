#pragma once
#include "../../src/VideoCatalog.h"
#include <algorithm>

struct MachineMenuGroup
{
    const char* code;
    const char* label;
    SystemModel defaultModel;
    std::vector<SystemModel> models;
};

inline const std::vector<MachineMenuGroup>& GetMachineMenuGroups()
{
    static const std::vector<MachineMenuGroup> groups = {
        {"SC3000", "SC-3000", SystemModel::SC3000Export, {SystemModel::SC3000Export, SystemModel::SC3000ExportSF7000, SystemModel::SC3000Japan, SystemModel::SC3000JapanSF7000}},
        {"SG1000", "SG-1000 Mark I/II", SystemModel::SG1000, {SystemModel::SG1000, SystemModel::SG1000SK1100}},
        {"MARK_III", "Mark III", SystemModel::SegaMarkIII, {SystemModel::SegaMarkIII}},
        {"MASTER_SYSTEM", "Master System", SystemModel::SMSNTSC, {SystemModel::SMSNTSC, SystemModel::SMSPAL}},
        {"GAME_GEAR", "Game Gear", SystemModel::GameGearExport, {SystemModel::GameGearJapan, SystemModel::GameGearExport}},
        {"MSX1", "MSX1", SystemModel::MSX1NTSC, {SystemModel::MSX1NTSC, SystemModel::MSX1PAL}},
        {"SORD_M5", "Sord M5", SystemModel::SordM5NTSC, {SystemModel::SordM5NTSC, SystemModel::SordM5PAL}},
        {"COLECO", "ColecoVision", SystemModel::ColecoVision, {SystemModel::ColecoVision, SystemModel::ColecoVisionSGM}}
    };
    return groups;
}
inline bool MachineGroupContains(const MachineMenuGroup& group, SystemModel model)
{
    return std::find(group.models.begin(), group.models.end(), model) != group.models.end();
}
inline const MachineMenuGroup& GetMachineMenuGroup(SystemModel model)
{
    for (const auto& group : GetMachineMenuGroups()) if (MachineGroupContains(group, model)) return group;
    return GetMachineMenuGroups().front();
}
inline bool MachineMenuModelAvailable(SystemModel model)
{
#if !GEARSF7000_ENABLE_AY
    if (IsMSXSystem(model) || HasSGM(model)) return false;
#endif
#if !GEARSF7000_ENABLE_SF7000
    if (IsSF7000System(model)) return false;
#endif
    return GetDefaultMachineVideoOption(model) != nullptr;
}
inline SystemModel ChooseMachineMenuModel(const MachineMenuGroup& group, SystemModel current, SystemModel remembered)
{
    for (auto model : {current, remembered, group.defaultModel})
        if (MachineGroupContains(group, model) && MachineMenuModelAvailable(model)) return model;
    for (auto model : group.models) if (MachineMenuModelAvailable(model)) return model;
    return group.defaultModel; // The menu disables a group with no available board.
}
