// SPDX-License-Identifier: GPL-3.0-or-later
#include "DebugEventCatalog.h"
#include <algorithm>
#include <limits>
#include <set>

namespace {
bool Fail(std::string& error, const std::string& message)
{
    error = message;
    return false;
}
bool Width(u8 width) { return width >= 1 && width <= 64; }
}

bool DebugEventCatalog::Add(const IDebugEventProvider& provider, std::string& error)
{
    return AddDefinition(provider.DescribeDebugDevice(), error);
}

bool DebugEventCatalog::AddDefinition(DebugDeviceDefinition device, std::string& error)
{
    error.clear();
    if (m_frozen) return Fail(error, "Catalog is frozen");
    if (device.key.empty() || device.typeKey.empty() || device.name.empty())
        return Fail(error, "Device requires key, type and name");
    if (m_definitions.size() >= std::numeric_limits<DebugDeviceId>::max())
        return Fail(error, "Device handle capacity exceeded");
    for (const auto& existing : m_definitions)
        if (existing.key == device.key) return Fail(error, "Duplicate device key: " + device.key);
    std::set<DebugSpaceId> ids;
    std::set<std::string> keys;
    constexpr u8 accesses = DebugEventAccess_Read | DebugEventAccess_Write |
        DebugEventAccess_Execute | DebugEventAccess_Event;
    for (const auto& space : device.spaces)
    {
        if (!space.id || space.key.empty() || space.name.empty() ||
            !ids.insert(space.id).second || !keys.insert(space.key).second)
            return Fail(error, "Invalid or duplicate space: " + device.key);
        if (space.targetMin > space.targetMax || (!Width(space.defaultBitWidth) && !(space.defaultBitWidth == 0 &&
             space.beforeValueAccesses == 0 && space.afterValueAccesses == 0)) ||
            (space.supportedAccesses & ~accesses) ||
            (space.beforeValueAccesses & ~space.supportedAccesses) ||
            (space.afterValueAccesses & ~space.supportedAccesses))
            return Fail(error, "Invalid space capabilities: " + space.key);
        std::set<u32> targets;
        for (const auto& target : space.targets)
            if (target.name.empty() || !Width(target.bitWidth) ||
                target.id < space.targetMin || target.id > space.targetMax ||
                !targets.insert(target.id).second)
                return Fail(error, "Invalid target: " + space.key);
    }
    std::set<std::string> presetKeys;
    for (const auto& preset : device.presets)
    {
        if (preset.key.empty() || preset.name.empty() ||
            preset.spaceKey.empty() || preset.targetStart > preset.targetEnd ||
            !presetKeys.insert(preset.key).second)
            return Fail(error, "Invalid or duplicate preset: " + device.key);
        const auto space = std::find_if(device.spaces.begin(), device.spaces.end(),
            [&](const DebugSpaceDefinition& candidate)
            { return candidate.key == preset.spaceKey; });
        if (space == device.spaces.end() ||
            preset.targetStart < space->targetMin ||
            preset.targetEnd > space->targetMax ||
            !preset.accessMask ||
            (preset.accessMask & ~space->supportedAccesses) != 0)
            return Fail(error, "Invalid preset target: " + preset.key);
    }
    m_definitions.push_back(std::move(device));
    return true;
}

bool DebugEventCatalog::Freeze(std::string& error)
{
    error.clear();
    if (m_frozen) return true;
    const size_t count = m_definitions.size();
    std::vector<DebugDeviceId> parents(count, 0);
    for (size_t i = 0; i < count; ++i)
    {
        const auto& key = m_definitions[i].parentKey;
        if (key.empty()) continue;
        for (size_t j = 0; j < count; ++j)
            if (m_definitions[j].key == key) parents[i] = static_cast<DebugDeviceId>(j + 1);
        if (!parents[i]) return Fail(error, "Missing parent: " + key);
    }
    for (size_t i = 0; i < count; ++i)
    {
        size_t steps = 0;
        auto parent = parents[i];
        while (parent)
        {
            if (++steps > count) return Fail(error, "Device hierarchy contains a cycle");
            parent = parents[parent - 1];
        }
    }
    m_targets.resize(count);
    m_spaces.resize(count);
    m_presets.resize(count);
    m_devices.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        const auto& definition = m_definitions[i];
        m_targets[i].resize(definition.spaces.size());
        m_spaces[i].reserve(definition.spaces.size());
        for (size_t j = 0; j < definition.spaces.size(); ++j)
        {
            const auto& space = definition.spaces[j];
            auto& targets = m_targets[i][j];
            targets.reserve(space.targets.size());
            for (const auto& target : space.targets)
                targets.push_back({target.id, target.name.c_str(), target.bitWidth});
            m_spaces[i].push_back({space.id, space.key.c_str(), space.name.c_str(),
                space.targetMin, space.targetMax, space.supportedAccesses,
                space.beforeValueAccesses, space.afterValueAccesses,
                space.defaultBitWidth, space.capabilities, targets.data(), targets.size()});
        }
        auto& presets = m_presets[i];
        presets.reserve(definition.presets.size());
        for (const auto& preset : definition.presets)
            presets.push_back({preset.key.c_str(), preset.name.c_str(),
                preset.spaceKey.c_str(), preset.targetStart, preset.targetEnd,
                preset.accessMask, preset.useValueCondition,
                preset.valueCondition, preset.valueMask,
                preset.beforeValueExpected, preset.valueExpected,
                preset.actions});
        m_devices.push_back({static_cast<DebugDeviceId>(i + 1), parents[i],
            definition.key.c_str(), definition.typeKey.c_str(), definition.name.c_str(),
            m_spaces[i].data(), m_spaces[i].size(), definition.traits,
            presets.data(), presets.size(), definition.formatter});
    }
    m_registry.SetCatalog(m_devices.data(), m_devices.size());
    m_frozen = true;
    return true;
}

const DebugDeviceRegistry* DebugEventCatalog::GetRegistry() const
{
    return m_frozen ? &m_registry : nullptr;
}

// Copies the legacy declaration; no descriptor storage is borrowed.
bool DebugEventCatalog::AddLegacyDevice(const DebugDeviceRegistry& registry,
                                        DebugDeviceId id, std::string& error)
{
    const auto* source = registry.FindDevice(id);
    if (!source) return Fail(error, "Missing legacy device");
    struct Adapter final : IDebugEventProvider
    {
        DebugDeviceDefinition definition;
        DebugDeviceDefinition DescribeDebugDevice() const override { return definition; }
    } adapter;
    auto& d = adapter.definition;
    d.key = source->key;
    d.typeKey = source->typeKey;
    d.name = source->displayName;
    d.traits = source->traits;
    d.formatter = source->formatter;
    if (source->parentId)
    {
        const auto* parent = registry.FindDevice(source->parentId);
        if (!parent) return Fail(error, "Missing legacy parent");
        d.parentKey = parent->key;
    }
    for (size_t i = 0; i < source->spaceCount; ++i)
    {
        const auto& s = source->spaces[i];
        DebugSpaceDefinition space;
        space.id = s.id;
        space.key = s.key;
        space.name = s.name;
        space.targetMin = s.targetMin;
        space.targetMax = s.targetMax;
        space.supportedAccesses = s.supportedAccesses;
        space.beforeValueAccesses = s.beforeValueAccesses;
        space.afterValueAccesses = s.afterValueAccesses;
        space.defaultBitWidth = s.defaultBitWidth;
        space.capabilities = s.capabilities;
        for (size_t t = 0; t < s.targetCount; ++t)
            space.targets.push_back({s.targets[t].id, s.targets[t].name, s.targets[t].bitWidth});
        d.spaces.push_back(std::move(space));
    }
    return Add(adapter, error);
}
