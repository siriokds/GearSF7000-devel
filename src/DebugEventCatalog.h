// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DEBUG_EVENT_CATALOG_H
#define DEBUG_EVENT_CATALOG_H
#include "IDebugEventProvider.h"

// One immutable catalog per composition. It must outlive its managers and
// captured events. Live replacement/history migration is deliberately not
// implemented by mutating this object or reusing its handles.
class DebugEventCatalog
{
public:
    DebugEventCatalog() = default;
    DebugEventCatalog(const DebugEventCatalog&) = delete;
    DebugEventCatalog& operator=(const DebugEventCatalog&) = delete;
    DebugEventCatalog(DebugEventCatalog&&) = delete;
    DebugEventCatalog& operator=(DebugEventCatalog&&) = delete;

    bool Add(const IDebugEventProvider& provider, std::string& error);
    bool AddDefinition(DebugDeviceDefinition definition, std::string& error);
    // Temporary adapter for components whose descriptions have not moved yet.
    bool AddLegacyDevice(const DebugDeviceRegistry& registry,
                         DebugDeviceId device, std::string& error);
    bool Freeze(std::string& error);
    // NULL until Freeze succeeds; never silently falls back to a machine catalog.
    const DebugDeviceRegistry* GetRegistry() const;
private:
    std::vector<DebugDeviceDefinition> m_definitions;
    std::vector<std::vector<std::vector<DebugTargetDescriptor>>> m_targets;
    std::vector<std::vector<DebugSpaceDescriptor>> m_spaces;
    std::vector<std::vector<DebugRulePresetDescriptor>> m_presets;
    std::vector<DebugDeviceDescriptor> m_devices;
    DebugDeviceRegistry m_registry;
    bool m_frozen = false;
};
#endif
