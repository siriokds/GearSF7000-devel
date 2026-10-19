/* GearSF7000 configuration ownership and historical value conversion.
 * GPL-3.0-or-later. This module never changes running hardware. */
#pragma once
#include "ConfigRepository.h"
#include "../../src/ISystemBus.h"

namespace config_profiles {
struct Profile { SystemModel model; const char* section; const char* key; };
inline constexpr Profile profiles[] = {
    {SystemModel::SG1000, "Machine.SG1000", "VDP.SG1000"},
    {SystemModel::SG1000SK1100, "Machine.SG1000", "VDP.SG1000SK1100"},
    {SystemModel::SC3000Export, "Machine.SC3000", "VDP.Export"},
    {SystemModel::SC3000ExportSF7000, "Machine.SC3000", "VDP.ExportSF7000"},
    {SystemModel::SC3000Japan, "Machine.SC3000", "VDP.Japan"},
    {SystemModel::SC3000JapanSF7000, "Machine.SC3000", "VDP.JapanSF7000"},
    {SystemModel::SMSNTSC, "Machine.SMS", "VDP.NTSC"},
    {SystemModel::SMSPAL, "Machine.SMS", "VDP.PAL"},
    {SystemModel::SegaMarkIII, "Machine.SMS", "VDP.MarkIII"},
    {SystemModel::GameGearJapan, "Machine.GameGear", "VDP.Japan"},
    {SystemModel::GameGearExport, "Machine.GameGear", "VDP.Export"},
    {SystemModel::ColecoVision, "Machine.Coleco", "VDP.Standard"},
    {SystemModel::ColecoVisionSGM, "Machine.Coleco", "VDP.SGM"},
    {SystemModel::MSX1NTSC, "Machine.MSX", "VDP.NTSC"},
    {SystemModel::MSX1PAL, "Machine.MSX", "VDP.PAL"},
    {SystemModel::SordM5NTSC, "Machine.SordM5", "VDP.NTSC"},
    {SystemModel::SordM5PAL, "Machine.SordM5", "VDP.PAL"}
};
inline const char* familySection(SystemModel model) {
    return IsM5System(model) ? "Machine.SordM5" : IsMSXSystem(model) ? "Machine.MSX" : IsColecoSystem(model) ? "Machine.Coleco" :
        IsGameGearSystem(model) ? "Machine.GameGear" : IsSMSSystem(model) ? "Machine.SMS" : "Machine.SG1000";
}
inline int integer(const config_storage::Ini& data, const char* section, const char* key, int fallback) {
    try {
        const auto value = data.get(section).get(key);
        size_t end = 0; int result = std::stoi(value, &end);
        return end == value.size() ? result : fallback;
    } catch (...) { return fallback; }
}
inline void migrate(config_storage::Ini& data) {
    const int id = integer(data, "Emulator", "Machine", static_cast<int>(SystemModel::SC3000Export));
    SystemModel model = id >= 0 && id <= static_cast<int>(SystemModel::MSX1PAL)
        ? static_cast<SystemModel>(id) : SystemModel::SC3000Export;
    const char* section = familySection(model);
    for (const char* key : {"MapperMode", "MapperType"}) {
        if (!data.get(section).has(key) && data.get("Emulator").has(key))
            data[section][key] = data.get("Emulator").get(key);
        data["Emulator"].remove(key);
    }
    for (const auto& p : profiles) if (p.model == model) {
        if (!data.get(p.section).has(p.key)) {
            const char* key = data.get("Emulator").has("VDP") ? "VDP" : "Region";
            if (data.get("Emulator").has(key)) data[p.section][p.key] = data.get("Emulator").get(key);
        }
    }
    if (!data.get(section).has("LastModel")) data[section]["LastModel"] = std::to_string(static_cast<int>(model));
    data["Emulator"].remove("VDP"); data["Emulator"].remove("Region");
    for (const char* key : {"AYEnable", "AYChip", "AYPortBase", "YM2413Enable"}) {
        const char* dest = std::string(key) == "YM2413Enable" ? "Machine.SMS" : "Machine.SegaAudio";
        if (!data.get(dest).has(key) && data.get("Audio").has(key)) data[dest][key] = data.get("Audio").get(key);
        data["Audio"].remove(key);
    }
}
} // namespace config_profiles
