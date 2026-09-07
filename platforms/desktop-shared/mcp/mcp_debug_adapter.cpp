#include "mcp_config.h"
#if GEARSF7000_ENABLE_MCP
#include "mcp_debug_adapter.h"
#include "mcp_debug_event_query.h"
#include "../emu.h"
#include "../application.h"
#include "../scheduler.h"
#include <cmath>
#include "../config.h"
#if GEARSF7000_ENABLE_SF7000
#include "../../../src/SF7000.h"
#endif
#if GEARSF7000_ENABLE_SR1000
#include "../../../src/SR1000.h"
#endif
#include "../../../src/Video.h"
#if GEARSF7000_ENABLE_AY
// For GetAyPortBase(), so get_ay8910_status can report where the card answers.
#include "../../../src/MachineIOPorts.h"
#endif
#include "../../../src/Z80Disassembler.h"
#include "../imgui/memory_editor.h"
#include "../gui_debug_disassembler_export.h"
#include "../gui_debug_basic_typer.h"
#include "../gui_debug.h"
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include "../rewind.h"
#include "../renderer.h"
#include "../config.h"

namespace { enum { MCP_BP_ROMRAM = 0, MCP_BP_VRAM = 1, MCP_BP_VDPREG = 2 }; json unavailable(const char* feature) { return {{"error", std::string(feature) + " is not available in this GearSF7000 build"}}; } }

namespace
{
const char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const unsigned char* data, int size)
{
    std::string result;
    result.reserve(static_cast<size_t>((size + 2) / 3) * 4);

    int i = 0;
    while (i < size)
    {
        unsigned char byte1 = data[i++];
        bool hasByte2 = i < size;
        unsigned char byte2 = hasByte2 ? data[i++] : 0;
        bool hasByte3 = i < size;
        unsigned char byte3 = hasByte3 ? data[i++] : 0;

        result.push_back(kBase64Chars[byte1 >> 2]);
        result.push_back(kBase64Chars[((byte1 & 0x03) << 4) | (byte2 >> 4)]);
        result.push_back(hasByte2 ? kBase64Chars[((byte2 & 0x0F) << 2) | (byte3 >> 6)] : '=');
        result.push_back(hasByte3 ? kBase64Chars[byte3 & 0x3F] : '=');
    }
    return result;
}
}

void DebugAdapter::Pause() { emu_pause(); }
void DebugAdapter::Resume() { emu_debug_continue(); }
void DebugAdapter::StepInto() { emu_debug_step_into(); }
void DebugAdapter::StepOver() { emu_debug_step_over(); }
void DebugAdapter::StepOut() { /* unavailable until the CPU-neutral call stack lands */ }
void DebugAdapter::StepFrame(int frames) { emu_debug_step_frames(frames); }

// The synchronous path. Runs the frames before answering, so the reply is the
// confirmation and there is nothing to poll for.
json DebugAdapter::StepFrameSync(int frames)
{
    const std::uint64_t before =
        m_core->GetVideo() ? m_core->GetVideo()->GetFrameRenderDiagnostics().frameSerial
                           : 0;

    bool breakpointHit = false;
    const int completed = emu_debug_step_frames_now(frames, &breakpointHit);

    const std::uint64_t after =
        m_core->GetVideo() ? m_core->GetVideo()->GetFrameRenderDiagnostics().frameSerial
                           : 0;

    json result = {
        {"success", true},
        {"mode", "sync"},
        {"pending", false},
        {"frames_requested", frames},
        {"frames_completed", completed},
        {"breakpoint_hit", breakpointHit},
        {"frame_serial_before", before},
        {"frame_serial_after", after},
        {"paused", true}
    };

    const CpuStateSnapshot cpu = m_core->GetCpuStateAccess()->GetCpuStateSnapshot();
    char pc[8];
    snprintf(pc, sizeof(pc), "%04X", cpu.pc);
    result["pc"] = pc;
    return result;
}
void DebugAdapter::Reset(bool paused) { m_core->ResetROM(NULL, paused); }
json DebugAdapter::GetDebugStatus() { const CpuStateSnapshot s=m_core->GetCpuStateAccess()->GetCpuStateSnapshot(); const bool corePaused=m_core->IsPaused(); const bool debuggerPaused=emu_is_debugging(); return {{"paused", corePaused || debuggerPaused}, {"paused_by_core", corePaused}, {"paused_by_debugger", debuggerPaused}, {"pc", s.pc}, {"halted", s.halted}, {"cpu_backend", m_core->GetCpuBackendName()}, {"save_states_available", m_core->IsCpuStatePersistenceAvailable()}}; }
json DebugAdapter::RunToAddress(u16 address) { m_core->GetMemory()->ArmRunToAddress(address); emu_debug_continue(); return {{"success",true},{"address",address},{"resumed",true}}; }

namespace
{
const char* debug_category_name(DebugEventCategory category)
{
    switch (category)
    {
    case DebugEventCategory::CpuExecute: return "cpu_execute";
    case DebugEventCategory::CpuMemory: return "cpu_memory";
    case DebugEventCategory::Vram: return "vram";
    case DebugEventCategory::Io: return "io";
    case DebugEventCategory::VdpRegister: return "vdp_register";
    case DebugEventCategory::Fdc: return "fdc";
    case DebugEventCategory::Ppi: return "ppi";
    case DebugEventCategory::VideoTiming: return "video_timing";
    case DebugEventCategory::Tape: return "tape";
    case DebugEventCategory::Audio: return "audio";
    case DebugEventCategory::AyExpansion: return "ay_expansion";
    case DebugEventCategory::DeviceState: return "device_state";
    default: return "unknown";
    }
}

bool is_vdp_latch_replacement(const DebugEvent& event)
{
    const DebugDeviceDescriptor* device = FindDebugDeviceDescriptor(event.device);
    return device != NULL && device->typeKey != NULL
        && std::string(device->typeKey) == "ti.tms9918"
        && event.space == DebugCommonSpace::State
        && event.targetV2 == Video::DebugCpuPortLatchReplacement;
}

json decode_vdp_port_request(u64 packed)
{
    return {
        {"write", (packed & (1u << 22)) != 0},
        {"address", (packed >> 8) & 0x3FFF},
        {"value", packed & 0xFF}
    };
}

bool debug_category_from_name(const std::string& name, DebugEventCategory* category)
{
    if (name == "cpu_memory") { *category = DebugEventCategory::CpuMemory; return true; }
    if (name == "cpu_execute") { *category = DebugEventCategory::CpuExecute; return true; }
    if (name == "io") { *category = DebugEventCategory::Io; return true; }
    if (name == "vram") { *category = DebugEventCategory::Vram; return true; }
    if (name == "vdp_register") { *category = DebugEventCategory::VdpRegister; return true; }
    if (name == "video_timing") { *category = DebugEventCategory::VideoTiming; return true; }
    if (name == "ppi") { *category = DebugEventCategory::Ppi; return true; }
    if (name == "fdc") { *category = DebugEventCategory::Fdc; return true; }
    if (name == "tape") { *category = DebugEventCategory::Tape; return true; }
    if (name == "audio") { *category = DebugEventCategory::Audio; return true; }
#if GEARSF7000_ENABLE_AY
    if (name == "ay_expansion") { *category = DebugEventCategory::AyExpansion; return true; }
#endif
    return false; // Other categories become available with their corresponding hooks.
}

const char* debug_value_condition_name(DebugValueCondition condition)
{
    switch (condition)
    {
    case DebugValueCondition::Any: return "any";
    case DebugValueCondition::AfterMaskedEqual: return "after_masked_equal";
    case DebugValueCondition::BeforeMaskedEqual: return "before_masked_equal";
    case DebugValueCondition::BeforeAndAfterMaskedEqual: return "before_and_after_masked_equal";
    case DebugValueCondition::BeforeNotEqualAfter: return "changed";
    case DebugValueCondition::AfterLessThanBefore: return "after_less_than_before";
    case DebugValueCondition::AfterLessOrEqualBefore: return "after_less_or_equal_before";
    case DebugValueCondition::AfterGreaterThanBefore: return "after_greater_than_before";
    case DebugValueCondition::AfterGreaterOrEqualBefore: return "after_greater_or_equal_before";
    case DebugValueCondition::MaskedBitsChanged: return "masked_bits_changed";
    case DebugValueCondition::BeforeMaskedSet: return "before_masked_set";
    case DebugValueCondition::AfterMaskedSet: return "after_masked_set";
    case DebugValueCondition::BeforeMaskedClear: return "before_masked_clear";
    case DebugValueCondition::AfterMaskedClear: return "after_masked_clear";
    }
    return "any";
}

bool debug_value_condition_from_name(const std::string& name, DebugValueCondition* condition)
{
    if (name.empty() || name == "any") { *condition = DebugValueCondition::Any; return true; }
    if (name == "after_masked_equal") { *condition = DebugValueCondition::AfterMaskedEqual; return true; }
    if (name == "before_masked_equal") { *condition = DebugValueCondition::BeforeMaskedEqual; return true; }
    if (name == "before_and_after_masked_equal") { *condition = DebugValueCondition::BeforeAndAfterMaskedEqual; return true; }
    if (name == "changed") { *condition = DebugValueCondition::BeforeNotEqualAfter; return true; }
    if (name == "after_less_than_before") { *condition = DebugValueCondition::AfterLessThanBefore; return true; }
    if (name == "after_less_or_equal_before") { *condition = DebugValueCondition::AfterLessOrEqualBefore; return true; }
    if (name == "after_greater_than_before") { *condition = DebugValueCondition::AfterGreaterThanBefore; return true; }
    if (name == "after_greater_or_equal_before") { *condition = DebugValueCondition::AfterGreaterOrEqualBefore; return true; }
    if (name == "masked_bits_changed") { *condition = DebugValueCondition::MaskedBitsChanged; return true; }
    if (name == "before_masked_set") { *condition = DebugValueCondition::BeforeMaskedSet; return true; }
    if (name == "after_masked_set") { *condition = DebugValueCondition::AfterMaskedSet; return true; }
    if (name == "before_masked_clear") { *condition = DebugValueCondition::BeforeMaskedClear; return true; }
    if (name == "after_masked_clear") { *condition = DebugValueCondition::AfterMaskedClear; return true; }
    return false;
}

json debug_rule_json(const DebugRule& rule)
{
    json output = {{"id", rule.id}, {"enabled", rule.enabled}, {"category", debug_category_name(rule.category)},
        {"start", rule.addressStart}, {"end", rule.addressEnd}, {"read", (rule.accessMask & DebugEventAccess_Read) != 0},
        {"write", (rule.accessMask & DebugEventAccess_Write) != 0}, {"execute", (rule.accessMask & DebugEventAccess_Execute) != 0},
        {"event", (rule.accessMask & DebugEventAccess_Event) != 0},
        {"pause", (rule.actions & DebugEventAction_Pause) != 0}, {"log", (rule.actions & DebugEventAction_Log) != 0},
        {"value_condition", rule.useValueCondition}, {"condition", debug_value_condition_name(rule.valueCondition)},
        {"value_mask", rule.valueMask}, {"value_expected", rule.valueExpected}, {"before_value_expected", rule.beforeValueExpected},
        {"break_on_hit", rule.breakOnHit}, {"hit_count", rule.hitCount}, {"one_shot", rule.oneShot}};
    if (rule.useProbe)
    {
        output["schema_version"] = 2;
        output["device"] = rule.deviceKey;
        output["space"] = rule.spaceKey;
    }
    return output;
}
}

json DebugAdapter::AddDebugRule(const std::string& categoryName, u16 start, u16 end, bool read, bool write, bool execute, bool event,
                                bool pause, bool log, bool valueCondition, u8 valueMask, u8 valueExpected, u64 breakOnHit,
                                const std::string& conditionName, bool oneShot, u8 beforeValueExpected)
{
    DebugEventCategory category;
    if (!debug_category_from_name(categoryName, &category))
        return {{"error", "Unsupported category in this build"}};
    if (start > end || (!read && !write && !execute && !event) || (!pause && !log))
        return {{"error", "Invalid rule condition or action"}};
    DebugValueCondition condition = DebugValueCondition::Any;
    if (!debug_value_condition_from_name(conditionName, &condition))
        return {{"error", "Unsupported value condition"}};
    if (condition == DebugValueCondition::Any && valueCondition)
        condition = DebugValueCondition::AfterMaskedEqual;

    DebugRule rule;
    rule.category = category;
    rule.addressStart = start;
    rule.addressEnd = end;
    rule.accessMask = (read ? DebugEventAccess_Read : 0) | (write ? DebugEventAccess_Write : 0) |
        (execute ? DebugEventAccess_Execute : 0) | (event ? DebugEventAccess_Event : 0);
    rule.useValueCondition = valueCondition;
    rule.valueMask = valueMask;
    rule.valueExpected = valueExpected;
    rule.beforeValueExpected = beforeValueExpected;
    rule.valueCondition = condition;
    rule.breakOnHit = breakOnHit;
    rule.oneShot = oneShot;
    rule.actions = (log ? DebugEventAction_Log : 0) | (pause ? DebugEventAction_Pause : 0);

    if (const char* error = ValidateDebugRule(rule))
        return {{"error", error}};

    DebugEventManager* events = m_core->GetMemory()->GetDebugEventManager();
    const u32 id = events->AddRule(rule);
    if (id == 0)
        return {{"error", "Rule was rejected by the debug engine"}};
    for (const DebugRule& added : events->GetRules())
        if (added.id == id)
            return {{"success", true}, {"rule", debug_rule_json(added)}};
    return {{"error", "Rule was not retained"}};
}

json DebugAdapter::AddDeviceDebugRule(const std::string& deviceKey, const std::string& spaceKey,
                                      u32 start, u32 end, bool read, bool write,
                                      bool execute, bool event, bool pause, bool log,
                                      u64 valueMask, u64 valueExpected, u64 breakOnHit,
                                      const std::string& conditionName, bool oneShot,
                                      u64 beforeValueExpected)
{
    if (deviceKey.empty() || spaceKey.empty() || start > end
        || (!read && !write && !execute && !event) || (!pause && !log))
        return {{"error", "Invalid device rule target, condition or action"}};
    DebugValueCondition condition = DebugValueCondition::Any;
    if (!debug_value_condition_from_name(conditionName, &condition))
        return {{"error", "Unsupported value condition"}};

    DebugRule rule;
    rule.useProbe = true;
    rule.deviceKey = deviceKey;
    rule.spaceKey = spaceKey;
    rule.category = DebugEventCategory::DeviceState;
    rule.addressStart = start;
    rule.addressEnd = end;
    rule.accessMask = (read ? DebugEventAccess_Read : 0)
        | (write ? DebugEventAccess_Write : 0)
        | (execute ? DebugEventAccess_Execute : 0)
        | (event ? DebugEventAccess_Event : 0);
    rule.valueCondition = condition;
    rule.useValueCondition = condition != DebugValueCondition::Any;
    rule.valueMask = valueMask;
    rule.valueExpected = valueExpected;
    rule.beforeValueExpected = beforeValueExpected;
    rule.breakOnHit = breakOnHit;
    rule.oneShot = oneShot;
    rule.actions = (log ? DebugEventAction_Log : 0) | (pause ? DebugEventAction_Pause : 0);

    DebugEventManager* events = m_core->GetMemory()->GetDebugEventManager();
    const u32 id = events->AddRule(rule);
    if (id == 0)
        return {{"error", "Rule was rejected; verify list_debug_devices capabilities"}};
    for (const DebugRule& added : events->GetRules())
        if (added.id == id)
            return {{"success", true}, {"rule", debug_rule_json(added)}};
    return {{"error", "Rule was not retained"}};
}

json DebugAdapter::ListDebugRules()
{
    json rules = json::array();
    for (const DebugRule& rule : m_core->GetMemory()->GetDebugEventManager()->GetRules())
        rules.push_back(debug_rule_json(rule));
    return {{"rules", rules}};
}

json DebugAdapter::RemoveDebugRule(u32 id)
{
    return {{"success", m_core->GetMemory()->GetDebugEventManager()->RemoveRule(id)}, {"id", id}};
}

json DebugAdapter::ClearDebugRules()
{
    m_core->GetMemory()->GetDebugEventManager()->ClearRules();
    return {{"success", true}};
}

json DebugAdapter::ListDebugDevices()
{
    const DebugDeviceRegistry& registry = GetGearDebugDeviceRegistry();
    size_t deviceCount = 0;
    const DebugDeviceDescriptor* devices = registry.GetDevices(&deviceCount);
    json output = json::array();
    for (size_t i = 0; i < deviceCount; ++i)
    {
        const DebugDeviceDescriptor& device = devices[i];
        json spaces = json::array();
        for (size_t s = 0; s < device.spaceCount; ++s)
        {
            const DebugSpaceDescriptor& space = device.spaces[s];
            json targets = json::array();
            for (size_t t = 0; t < space.targetCount; ++t)
            {
                targets.push_back({
                    {"id", space.targets[t].id},
                    {"name", space.targets[t].name},
                    {"bit_width", space.targets[t].bitWidth}
                });
            }
            spaces.push_back({
                {"id", space.id}, {"key", space.key}, {"name", space.name},
                {"target_min", space.targetMin}, {"target_max", space.targetMax},
                {"default_bit_width", space.defaultBitWidth},
                {"read", (space.supportedAccesses & DebugEventAccess_Read) != 0},
                {"write", (space.supportedAccesses & DebugEventAccess_Write) != 0},
                {"execute", (space.supportedAccesses & DebugEventAccess_Execute) != 0},
                {"event", (space.supportedAccesses & DebugEventAccess_Event) != 0},
                {"events_published", (space.capabilities & DebugDescriptorCapability_EventsPublished) != 0},
                {"snapshot_readable", (space.capabilities & DebugDescriptorCapability_SnapshotReadable) != 0},
                {"mutable", (space.capabilities & DebugDescriptorCapability_Mutable) != 0},
                {"targets", targets}
            });
        }
        const DebugDeviceDescriptor* parent = registry.GetParent(device.id);
        output.push_back({
            {"id", device.id}, {"key", device.key}, {"type_key", device.typeKey},
            {"name", device.displayName},
            {"parent", parent != NULL ? json(parent->key) : json(nullptr)},
            {"child_count", registry.GetChildCount(device.id)},
            {"spaces", spaces}
        });
    }
    return {{"schema_version", 2}, {"devices", output}};
}

json DebugAdapter::GetDebugEvents(const json& arguments)
{
    std::vector<DebugEvent> events;
    DebugEventManager* manager = m_core->GetMemory()->GetDebugEventManager();
    manager->CopyEvents(events);

    McpDebugEventQuery query;
    query.limit = static_cast<size_t>(arguments.value("count", 128));
    query.newestFirst = arguments.value("order", std::string("oldest_first")) == "newest_first";
#define MCP_EVENT_RANGE(name, flag, member, type) do { \
    if (arguments.contains(name)) { query.flag = true; query.member = arguments[name].get<type>(); } \
} while (0)
    MCP_EVENT_RANGE("sequence_from", hasSequenceFrom, sequenceFrom, u64);
    MCP_EVENT_RANGE("sequence_to", hasSequenceTo, sequenceTo, u64);
    MCP_EVENT_RANGE("clock_from", hasClockFrom, clockFrom, u64);
    MCP_EVENT_RANGE("clock_to", hasClockTo, clockTo, u64);
    MCP_EVENT_RANGE("pc_start", hasPcStart, pcStart, u16);
    MCP_EVENT_RANGE("pc_end", hasPcEnd, pcEnd, u16);
    MCP_EVENT_RANGE("target_start", hasTargetStart, targetStart, u32);
    MCP_EVENT_RANGE("target_end", hasTargetEnd, targetEnd, u32);
#undef MCP_EVENT_RANGE

    if ((query.hasSequenceFrom && query.hasSequenceTo && query.sequenceFrom > query.sequenceTo) ||
        (query.hasClockFrom && query.hasClockTo && query.clockFrom > query.clockTo) ||
        (query.hasPcStart && query.hasPcEnd && query.pcStart > query.pcEnd) ||
        (query.hasTargetStart && query.hasTargetEnd && query.targetStart > query.targetEnd))
        return {{"error", "Invalid debug-event range: start/from must not exceed end/to"}};

    if (arguments.contains("category"))
    {
        DebugEventCategory category;
        const std::string categoryName = arguments["category"].get<std::string>();
        bool validCategory = debug_category_from_name(categoryName, &category);
        if (!validCategory && categoryName == "device_state")
        {
            category = DebugEventCategory::DeviceState;
            validCategory = true;
        }
        if (!validCategory)
            return {{"error", "Unknown debug-event category"}};
        query.category = static_cast<int>(category);
    }
    if (arguments.contains("access"))
    {
        const std::string access = arguments["access"];
        if (access == "read") query.accessMask = DebugEventAccess_Read;
        else if (access == "write") query.accessMask = DebugEventAccess_Write;
        else if (access == "execute") query.accessMask = DebugEventAccess_Execute;
        else if (access == "event") query.accessMask = DebugEventAccess_Event;
    }
    query.device = arguments.value("device", std::string());
    query.space = arguments.value("space", std::string());

    std::vector<DebugEvent> filtered;
    const size_t matched = McpFilterDebugEvents(events, query, filtered);
    json output = json::array();
    for (const DebugEvent& event : filtered)
    {
        json entry = {{"sequence", event.sequence}, {"clock", event.clock}, {"rule_id", event.ruleId},
            {"category", debug_category_name(static_cast<DebugEventCategory>(event.category))}, {"access", event.access},
            {"pc", event.pc}, {"target", event.target}, {"value", event.value}, {"value_known", event.valueKnown},
            {"before", event.beforeValue}, {"before_known", event.beforeKnown},
            {"after", event.afterValue}, {"after_known", event.afterKnown},
            {"schema_version", event.schemaVersion}, {"device", GetDebugDeviceName(event.device)},
            {"space", GetDebugSpaceName(event.space)}, {"target_v2", event.targetV2},
            {"value_width", event.valueWidth}, {"before_v2", event.beforeValueV2},
            {"after_v2", event.afterValueV2}, {"source", GetDebugEventSourceName(event.source)},
            {"phase", GetDebugEventPhaseName(event.phase)},
            {"pause", (event.actions & DebugEventAction_Pause) != 0}};
        if (event.bankKnown)
            entry["bank"] = event.bank;
        if (is_vdp_latch_replacement(event) && event.beforeKnown && event.afterKnown)
        {
            entry["vdp_latch_replacement"] = {
                {"discarded", decode_vdp_port_request(event.beforeValueV2)},
                {"replacement", decode_vdp_port_request(event.afterValueV2)}
            };
        }
        if (event.raster.rasterKnown)
        {
            entry["raster"] = {
                {"line", event.raster.line},
                {"dot", event.raster.dot},
                {"vertical_region",
                    GetDebugRasterRegionName(event.raster.verticalRegion)},
                {"horizontal_region",
                    GetDebugRasterRegionName(event.raster.horizontalRegion)},
                {"slot_calendar",
                    GetDebugVdpSlotCalendarName(event.raster.calendar)}
            };
        }
        if (event.raster.gapKnown)
        {
            // End-of-transfer to end-of-transfer, which is the spacing the
            // VDP itself sees. The TMS9918 needs 29T in Graphics I/II.
            entry["tstates_since_previous_port_access"] =
                event.raster.tStatesSincePreviousAccess;
        }
        output.push_back(entry);
    }
    return {{"events", output}, {"count", events.size()}, {"buffered", events.size()}, {"matched", matched},
            {"returned", filtered.size()}, {"dropped", manager->GetDroppedEventCount()},
            {"order", query.newestFirst ? "newest_first" : "oldest_first"}};
}

json DebugAdapter::ClearDebugEvents()
{
    m_core->GetMemory()->GetDebugEventManager()->ClearEvents();
    return {{"success", true}};
}

void DebugAdapter::SetBreakpoint(u16 a,int t,bool r,bool w,bool x) { SetBreakpointRange(a,a,t,r,w,x); }
void DebugAdapter::SetBreakpointRange(u16 a,u16 b,int t,bool r,bool w,bool x) { Memory* m=m_core->GetMemory(); if(t==MCP_BP_ROMRAM && x) { for(u16 p=a;;p++) { Memory::stDisassembleRecord* d=m->GetDisassembleRecord(p,true); if(std::find(m->GetBreakpointsCPU()->begin(),m->GetBreakpointsCPU()->end(),d)==m->GetBreakpointsCPU()->end()) m->GetBreakpointsCPU()->push_back(d); if(p==b) break; } } else { Memory::stMemoryBreakpoint bp={a,b,r,w,a!=b}; (t==MCP_BP_VRAM?m->GetBreakpointsVRAM():m->GetBreakpointsMem())->push_back(bp); } }
void DebugAdapter::ClearBreakpointByAddress(u16 a,int t,u16 b) { Memory* m=m_core->GetMemory(); if(t==MCP_BP_ROMRAM) { auto* v=m->GetBreakpointsCPU(); v->erase(std::remove_if(v->begin(),v->end(),[a](Memory::stDisassembleRecord* r){return r&&r->address==a;}),v->end()); } else { auto* v=t==MCP_BP_VRAM?m->GetBreakpointsVRAM():m->GetBreakpointsMem(); v->erase(std::remove_if(v->begin(),v->end(),[a,b](const Memory::stMemoryBreakpoint& x){return x.address1==a && (!b||x.address2==b);}),v->end()); } }
std::vector<BreakpointInfo> DebugAdapter::ListBreakpoints() { std::vector<BreakpointInfo> out; Memory* m=m_core->GetMemory(); for(auto* r:*m->GetBreakpointsCPU()) if(r) out.push_back({true,MCP_BP_ROMRAM,r->address,r->address,false,false,true,false,"rom_ram"}); for(int t=1;t<=2;t++) for(const auto& b:*(t==1?m->GetBreakpointsVRAM():m->GetBreakpointsMem())) out.push_back({true,t,b.address1,b.address2,b.read,b.write,false,b.range,t==1?"vram":"rom_ram"}); return out; }
RegistersSnapshot DebugAdapter::GetRegisters() { const CpuStateSnapshot s=m_core->GetCpuStateAccess()->GetCpuStateSnapshot(); return {s.af,s.bc,s.de,s.hl,s.af2,s.bc2,s.de2,s.hl2,s.ix,s.iy,s.sp,s.pc,s.wz,s.i,s.r,s.iff1,s.iff2,s.halted,s.interruptMode}; }
void DebugAdapter::SetRegister(const std::string& n,u32 v) { CpuStateAccess* cpu=m_core->GetCpuStateAccess(); if(n=="AF")cpu->SetCpuRegister(CpuRegister::AF,v); else if(n=="BC")cpu->SetCpuRegister(CpuRegister::BC,v); else if(n=="DE")cpu->SetCpuRegister(CpuRegister::DE,v); else if(n=="HL")cpu->SetCpuRegister(CpuRegister::HL,v); else if(n=="AF2")cpu->SetCpuRegister(CpuRegister::AF2,v); else if(n=="BC2")cpu->SetCpuRegister(CpuRegister::BC2,v); else if(n=="DE2")cpu->SetCpuRegister(CpuRegister::DE2,v); else if(n=="HL2")cpu->SetCpuRegister(CpuRegister::HL2,v); else if(n=="IX")cpu->SetCpuRegister(CpuRegister::IX,v); else if(n=="IY")cpu->SetCpuRegister(CpuRegister::IY,v); else if(n=="SP")cpu->SetCpuRegister(CpuRegister::SP,v); else if(n=="PC")cpu->SetCpuRegister(CpuRegister::PC,v); else if(n=="WZ")cpu->SetCpuRegister(CpuRegister::WZ,v); else if(n=="I")cpu->SetCpuRegister(CpuRegister::I,v); else if(n=="R")cpu->SetCpuRegister(CpuRegister::R,v); else if(n=="IFF1")cpu->SetCpuRegister(CpuRegister::IFF1,v); else if(n=="IFF2")cpu->SetCpuRegister(CpuRegister::IFF2,v); else if(n=="IM")cpu->SetCpuRegister(CpuRegister::InterruptMode,v); }
MemoryAreaInfo DebugAdapter::GetMemoryAreaInfo(int a)
{
    Memory* m = m_core->GetMemory();
    Video* v = m_core->GetVideo();
    Cartridge* cartridge = m_core->GetCartridge();
    switch (a)
    {
        case 0: return {0, "Z80 address space", 0x10000, 0, nullptr};
        case 1:
        {
            // The $C000-$FFFF work RAM window is backed by a different
            // buffer - and mirrored over a different size - depending on
            // the loaded cartridge/mode; this must mirror Memory::Read()'s
            // own dispatch (Memory_inline.h) exactly, or area 1 silently
            // shows a buffer the running game never touches. Confirmed live
            // on an SG1000_16K game (Pop Flamer): memory_search on the old
            // always-m_pRam exposure found zero changes over minutes of
            // real play, because that cartridge type's RAM is m_pSGMRam,
            // not m_pRam - area 0 (which reads through Memory::Read()) saw
            // the same address change on every hit. Unconditionally
            // exposing m_pRam as 0x4000 bytes was also an out-of-bounds
            // read/write past its real MAX_SRAM_SIZE (0x800) allocation for
            // every cartridge type, independent of that bug.
            if (m->IsSF7000Enabled())
                return {1, "RAM", 0x4000, 0xC000, m->GetSGMRam() + 0xC000};
            switch (cartridge->GetType())
            {
                case Cartridge::CartridgeTypes::SC3000_ASC16L:
                case Cartridge::CartridgeTypes::SC3000_32K:
                    return {1, "RAM", 0x4000, 0xC000, m->GetSGMRam() + 0x4000};
                case Cartridge::CartridgeTypes::SG1000_16K:
                    return {1, "RAM", 0x4000, 0xC000, m->GetSGMRam()};
                case Cartridge::CartridgeTypes::SC3000_2K:
                    return {1, "RAM", 0x800, 0xC000, m->GetRam()};
                default:
                    return {1, "RAM", 0x400, 0xC000, m->GetRam()};
            }
        }
        case 2: return {2, "BIOS", 0x2000, 0, m->GetBios()};
        case 3: return {3, "VRAM", 0x4000, 0, v->GetVRAM()};
        case 4:
            return {4, "Cartridge ROM", static_cast<u32>(cartridge->GetROMSize()), 0,
                cartridge->GetROM()};
        default: return {-1, "invalid", 0, 0, nullptr};
    }
}

std::vector<MemoryAreaInfo> DebugAdapter::ListMemoryAreas()
{
    return {GetMemoryAreaInfo(0), GetMemoryAreaInfo(1), GetMemoryAreaInfo(2),
        GetMemoryAreaInfo(3), GetMemoryAreaInfo(4)};
}
std::vector<u8> DebugAdapter::ReadMemoryArea(int a,u32 o,size_t n){std::vector<u8> out; MemoryAreaInfo i=GetMemoryAreaInfo(a); if(!i.size||o>=i.size)return out; n=std::min(n,(size_t)i.size-o); out.reserve(n); for(size_t x=0;x<n;x++)out.push_back(a==0?m_core->GetMemory()->Read((u16)(o+x)):i.data[o+x]); return out;}
void DebugAdapter::WriteMemoryArea(int a,u32 o,const std::vector<u8>& d){MemoryAreaInfo i=GetMemoryAreaInfo(a); if(!i.size||o>=i.size)return; size_t n=std::min(d.size(),(size_t)i.size-o); for(size_t x=0;x<n;x++){if(a==0)m_core->GetMemory()->Write((u16)(o+x),d[x]); else if(a==3)m_core->GetVideo()->WriteVRAM((u16)(o+x),d[x]); else if(i.data)i.data[o+x]=d[x];}}
std::vector<DisasmLine> DebugAdapter::GetDisassembly(u16 a,u16 b,int,bool){std::vector<DisasmLine> out; Memory* m=m_core->GetMemory(); Z80Disassembler* d=m_core->GetDisassembler(); if(!d)return out; for(u32 x=a;x<=b;){d->Disassemble((u16)x); auto* r=m->GetDisassembleRecord((u16)x,false); if(!r){x++;continue;} out.push_back({r->address,(u8)r->bank,r->name,r->bytes,r->segment,r->size,r->jump,r->jump_address,0,false,0,false,0}); x+=std::max(1,r->size);} return out;}

json DebugAdapter::GetCodeCoverage(const json& query)
{
    const bool detailed = query.value("detailed", false);
    const int offset = std::max(0, query.value("offset", 0));
    const int limit = std::clamp(query.value("limit", 512), 1, 4096);
    std::vector<Memory::CodeCoverageRecord> records =
        m_core->GetMemory()->GetCodeCoverage();
    std::sort(records.begin(), records.end(),
              [](const auto& a, const auto& b) {
                  if (a.segment != b.segment) return a.segment < b.segment;
                  if (a.bank != b.bank) return a.bank < b.bank;
                  return a.physical_address < b.physical_address;
              });

    json banks = json::object();
    u64 totalExecutions = 0;
    for (const auto& record : records)
    {
        totalExecutions += record.execution_count;
        const std::string key = record.segment + ":" + std::to_string(record.bank);
        json& bank = banks[key];
        if (bank.is_null())
            bank = {{"segment", record.segment}, {"bank", record.bank},
                    {"instructions", 0}, {"executions", 0},
                    {"first_physical", record.physical_address},
                    {"last_physical", record.physical_address}};
        bank["instructions"] = bank["instructions"].get<u64>() + 1;
        bank["executions"] = bank["executions"].get<u64>() + record.execution_count;
        bank["first_physical"] = std::min(bank["first_physical"].get<u32>(), record.physical_address);
        bank["last_physical"] = std::max(bank["last_physical"].get<u32>(), record.physical_address);
    }

    json result = {
        {"schema_version", 1},
        {"rom_crc32", m_core->GetCartridge()->GetCRC()},
        {"rom_size", m_core->GetCartridge()->GetROMSize()},
        {"covered_instructions", records.size()},
        {"total_executions", totalExecutions},
        {"banks", json::array()}
    };
    for (auto it = banks.begin(); it != banks.end(); ++it)
        result["banks"].push_back(it.value());

    if (detailed)
    {
        json entries = json::array();
        const size_t begin = std::min(records.size(), static_cast<size_t>(offset));
        const size_t end = std::min(records.size(), begin + static_cast<size_t>(limit));
        for (size_t i = begin; i < end; ++i)
        {
            const auto& r = records[i];
            entries.push_back({{"logical_address", r.logical_address},
                               {"physical_address", r.physical_address},
                               {"bank", r.bank}, {"segment", r.segment},
                               {"size", r.size}, {"bytes", r.bytes},
                               {"instruction", r.instruction},
                               {"execution_count", r.execution_count},
                               {"first_tstate", r.first_execution_tstate},
                               {"last_tstate", r.last_execution_tstate}});
        }
        result["entries"] = entries;
        result["offset"] = begin;
        result["returned"] = end - begin;
        result["has_more"] = end < records.size();
    }
    return result;
}

json DebugAdapter::ClearCodeCoverage()
{
    m_core->GetMemory()->ClearCodeCoverage();
    return {{"success", true}, {"covered_instructions", 0}};
}
json DebugAdapter::GetZ80Clock(){const std::uint64_t ticks=m_core->GetCpuStateAccess()->GetElapsedTStates(); const int clockHz=m_core->GetVideo()->IsPAL()?GC_MASTER_CLOCK_PAL:GC_MASTER_CLOCK_NTSC; const double clockMHz=static_cast<double>(clockHz)/1000000.0; const double microseconds=static_cast<double>(ticks)/clockMHz; return {{"ticks",ticks},{"clock_hz",clockHz},{"clock_mhz",clockMHz},{"region",m_core->GetVideo()->IsPAL()?"PAL":"NTSC"},{"microseconds",microseconds},{"milliseconds",microseconds/1000.0}};}
json DebugAdapter::ResetZ80Clock(){m_core->GetCpuStateAccess()->SetElapsedTStates(0); return GetZ80Clock();}
json DebugAdapter::GetZ80Status(){const CpuStateSnapshot s=m_core->GetCpuStateAccess()->GetCpuStateSnapshot(); RegistersSnapshot r=GetRegisters(); const u8 flags=(u8)(r.AF&0xFF); return {{"AF",r.AF},{"BC",r.BC},{"DE",r.DE},{"HL",r.HL},{"AF_alt",r.AF2},{"BC_alt",r.BC2},{"DE_alt",r.DE2},{"HL_alt",r.HL2},{"IX",r.IX},{"IY",r.IY},{"WZ",r.WZ},{"SP",r.SP},{"PC",r.PC},{"I",r.I},{"R",r.R},{"IFF1",r.IFF1},{"IFF2",r.IFF2},{"interrupt_mode",r.InterruptMode},{"int_requested",s.intLine},{"nmi_requested",s.nmiLine},{"halted",r.Halt},{"flags",{{"S",(flags&0x80)!=0},{"Z",(flags&0x40)!=0},{"Y",(flags&0x20)!=0},{"H",(flags&0x10)!=0},{"X",(flags&0x08)!=0},{"P_V",(flags&0x04)!=0},{"N",(flags&0x02)!=0},{"C",(flags&0x01)!=0}}},{"clock",GetZ80Clock()}};}

json DebugAdapter::GetAtomicSnapshot(const json& arguments)
{
    static const size_t kMaximumRanges = 16;
    static const size_t kMaximumRangeBytes = 4096;
    static const size_t kMaximumTotalBytes = 16384;

    const json ranges = arguments.value("memory_ranges", json::array());
    if (!ranges.is_array())
        return {{"error", "memory_ranges must be an array"}};
    if (ranges.size() > kMaximumRanges)
        return {{"error", "At most 16 memory ranges may be captured atomically"}};

    size_t totalBytes = 0;
    for (const json& range : ranges)
    {
        if (!range.is_object() || !range.contains("area") || !range.contains("offset") || !range.contains("size"))
            return {{"error", "Each memory range requires integer area, offset and size fields"}};
        if (!range["area"].is_number_integer() || !range["offset"].is_number_integer() ||
            !range["size"].is_number_integer() || range["area"].get<int64_t>() < 0 ||
            range["offset"].get<int64_t>() < 0 || range["size"].get<int64_t>() < 0)
            return {{"error", "Memory range area, offset and size must be non-negative integers"}};

        const int area = range["area"].get<int>();
        const u32 offset = range["offset"].get<u32>();
        const size_t size = range["size"].get<size_t>();
        const MemoryAreaInfo info = GetMemoryAreaInfo(area);
        if (info.id < 0 || info.size == 0)
            return {{"error", "Invalid or unavailable memory area"}, {"area", area}};
        if (size == 0 || size > kMaximumRangeBytes)
            return {{"error", "Each memory range must contain 1 to 4096 bytes"}};
        if (offset >= info.size || size > static_cast<size_t>(info.size - offset))
            return {{"error", "Memory range exceeds the selected area"}, {"area", area},
                    {"offset", offset}, {"size", size}, {"area_size", info.size}};
        totalBytes += size;
        if (totalBytes > kMaximumTotalBytes)
            return {{"error", "Atomic memory payload exceeds the 16384-byte total limit"}};
    }

    Video* video = m_core->GetVideo();
    const u64 clockBefore = m_core->GetCpuStateAccess()->GetElapsedTStates();
    const u64 frameBefore = video ? video->GetFrameRenderDiagnostics().frameSerial : 0;

    json result = {
        {"schema_version", 1},
        {"machine_generation", emu_get_machine_generation()},
        {"paused", m_core->IsPaused() || emu_is_debugging()},
        {"atomic_at_emulation_boundary", true}
    };
    if (arguments.value("cpu", true))
        result["cpu"] = GetZ80Status();
    if (arguments.value("vdp", true))
        result["vdp"] = {{"registers", GetVDPRegisters()["registers"]}, {"status", GetVDPStatus()}};
    if (arguments.value("sf7000", false))
        result["sf7000"] = GetSF7000Status();

    json memory = json::array();
    for (const json& range : ranges)
    {
        const int area = range["area"].get<int>();
        const u32 offset = range["offset"].get<u32>();
        const size_t size = range["size"].get<size_t>();
        const MemoryAreaInfo info = GetMemoryAreaInfo(area);
        const std::vector<u8> data = ReadMemoryArea(area, offset, size);
        std::ostringstream bytes;
        for (size_t i = 0; i < data.size(); ++i)
        {
            if (i != 0) bytes << ' ';
            bytes << std::hex << std::uppercase << std::setfill('0') << std::setw(2)
                  << static_cast<unsigned int>(data[i]);
        }
        memory.push_back({{"area", area}, {"name", info.name}, {"offset", offset},
                          {"size", data.size()}, {"data", bytes.str()}});
    }
    result["memory"] = memory;

    const u64 clockAfter = m_core->GetCpuStateAccess()->GetElapsedTStates();
    const u64 frameAfter = video ? video->GetFrameRenderDiagnostics().frameSerial : 0;
    result["capture"] = {{"clock_before", clockBefore}, {"clock_after", clockAfter},
                         {"frame_serial_before", frameBefore}, {"frame_serial_after", frameAfter},
                         {"stable", clockBefore == clockAfter && frameBefore == frameAfter}};
    return result;
}
json DebugAdapter::GetVDPRegisters(){json a=json::array();u8* r=m_core->GetVideo()->GetRegisters();for(int i=0;i<8;i++)a.push_back(r[i]);return {{"registers",a}};}
json DebugAdapter::GetVDPStatus()
{
    Video* v = m_core->GetVideo();
    const auto sequencer = v->GetVRAMSequencerSnapshot();
    const auto frameDiagnostics = v->GetFrameRenderDiagnostics();
    const auto kindName = [](TMS9918VramSequencer::RequestKind kind) {
        switch (kind)
        {
            case TMS9918VramSequencer::RequestKind::CpuRead: return "cpu_read";
            case TMS9918VramSequencer::RequestKind::CpuWrite: return "cpu_write";
            case TMS9918VramSequencer::RequestKind::VideoFetch: return "video_fetch";
            case TMS9918VramSequencer::RequestKind::Refresh: return "refresh";
            case TMS9918VramSequencer::RequestKind::Internal: return "internal";
            case TMS9918VramSequencer::RequestKind::SpriteFetch: return "sprite_fetch";
        }
        return "unknown";
    };
    const auto scheduleName = [](TMS9918VramSequencer::Schedule schedule) {
        switch (schedule)
        {
            case TMS9918VramSequencer::Schedule::Refresh: return "refresh";
            case TMS9918VramSequencer::Schedule::Graphics: return "graphics";
            case TMS9918VramSequencer::Schedule::Text: return "text";
            case TMS9918VramSequencer::Schedule::Multicolor:
                return "multicolor";
        }
        return "unknown";
    };

    return {
        {"status", v->GetStatusReg()},
        {"address", v->GetAddressReg()},
        {"address_counter", v->GetAddressReg()},
        {"read_buffer", v->GetBufferReg()},
        {"control_latch_first_byte", v->GetLatch()},
        {"control_latch_value", v->GetControlLatchValue()},
        {"mode", v->GetMode()},
        {"line", v->GetRenderLine()},
        {"column", v->GetColumn()},
        {"renderer_validation", {
            {"frame_serial", frameDiagnostics.frameSerial},
            {"legacy_frame_hash", frameDiagnostics.legacyFrameHash},
            {"visible_line_cpu_vram_writes",
                frameDiagnostics.visibleLineCpuVramWrites},
            {"visible_line_vdp_register_writes",
                frameDiagnostics.visibleLineVdpRegisterWrites},
            {"completed_background_fetches",
                frameDiagnostics.completedBackgroundFetches},
            {"completed_sprite_fetches",
                frameDiagnostics.completedSpriteFetches},
            {"completed_sprite_selection_scans",
                frameDiagnostics.completedSpriteSelectionScans},
            {"streaming_background_comparable",
                frameDiagnostics.streamingBackgroundComparable},
            {"streaming_background_compared_lines",
                frameDiagnostics.streamingBackgroundComparedLines},
            {"streaming_background_mismatched_pixels",
                frameDiagnostics.streamingBackgroundMismatchedPixels},
            {"streaming_background_hash",
                frameDiagnostics.streamingBackgroundHash},
            {"streaming_sprite_comparable",
                frameDiagnostics.streamingSpriteComparable},
            {"streaming_sprite_compared_lines",
                frameDiagnostics.streamingSpriteComparedLines},
            {"streaming_sprite_mismatched_pixels",
                frameDiagnostics.streamingSpriteMismatchedPixels},
            {"streaming_sprite_hash",
                frameDiagnostics.streamingSpriteHash},
            {"streaming_blank_first_line",
                frameDiagnostics.streamingBlankFirstLine},
            {"streaming_blank_first_column",
                frameDiagnostics.streamingBlankFirstColumn},
            {"sprite_status", {
                {"authoritative", frameDiagnostics.streamingStatusAuthoritative
                    ? "streaming" : "legacy"},
                {"legacy_overflow_line", frameDiagnostics.legacyOverflowLine},
                {"legacy_overflow_sprite", frameDiagnostics.legacyOverflowSprite},
                {"legacy_collisions", frameDiagnostics.legacyCollisions},
                {"streaming_overflow_line", frameDiagnostics.streamingOverflowLine},
                {"streaming_overflow_sprite", frameDiagnostics.streamingOverflowSprite},
                {"streaming_collision_line", frameDiagnostics.streamingCollisionLine},
                {"streaming_collision_dot", frameDiagnostics.streamingCollisionDot},
                {"streaming_collisions", frameDiagnostics.streamingCollisions},
                {"suppressed_vblank_flags", frameDiagnostics.suppressedVBlankFlags}
            }}
        }},
        {"vram_sequencer", {
            {"line", sequencer.position.line},
            {"phase", sequencer.position.phase},
            {"schedule", scheduleName(sequencer.schedule)},
            {"video_schedule", scheduleName(sequencer.videoSchedule)},
            {"minimum_latency_phases",
                TMS9918VramSequencer::CpuPortMinimumLatencyPhases},
            {"pending", sequencer.hasPendingRequest},
            {"pending_kind", kindName(sequencer.pendingRequest.kind)},
            {"pending_address", sequencer.pendingRequest.address},
            {"pending_value", sequencer.pendingRequest.value},
            {"pending_issued_line", sequencer.pendingIssuedAt.line},
            {"pending_issued_phase", sequencer.pendingIssuedAt.phase},
            {"latched", sequencer.hasQueuedRequest},
            {"latched_kind", kindName(sequencer.queuedRequest.kind)},
            {"latched_address", sequencer.queuedRequest.address},
            {"latched_value", sequencer.queuedRequest.value},
            {"latched_eligible_at", sequencer.queuedEligibleAt},
            {"port_attempts", sequencer.statistics.portAttempts},
            {"latch_replacements", sequencer.statistics.latchReplacements},
            {"issued_transfers", sequencer.statistics.issuedTransfers},
            {"completed_transfers", sequencer.statistics.completedTransfers}
        }}
    };
}

// Lowercase an ASCII tool argument in place. Source names are compared
// case-insensitively so "Streaming" and "streaming" both work.
static std::string LowerAscii(const std::string& text)
{
    std::string result = text;
    for (char& c : result)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

json DebugAdapter::GetSlotHistory()
{
    // Live per-scanline view of every DRAM slot the beam has reached so far
    // this raster line, all 171 of them across the full 342-dot line -
    // blanking included, not just the 256 visible pixels. Analogous to
    // C64 Debugger's badline/VIC-II bus access view, but keyed to the
    // TMS9918's slot grid: it never stalls the CPU, so what this shows is a
    // CPU slot either used or left idle, not a badline-style freeze.
    Video* v = m_core->GetVideo();
    const Video::SlotActivityRecord* history = v->GetLineSlotHistory();
    const auto kindName = [](TMS9918VramSlotSchedule::Activity activity) {
        switch (activity)
        {
            case TMS9918VramSlotSchedule::Activity::Cpu: return "cpu";
            case TMS9918VramSlotSchedule::Activity::Refresh: return "refresh";
            case TMS9918VramSlotSchedule::Activity::Name: return "name";
            case TMS9918VramSlotSchedule::Activity::Colour: return "colour";
            case TMS9918VramSlotSchedule::Activity::Pattern: return "pattern";
            case TMS9918VramSlotSchedule::Activity::SpriteScanY: return "sprite_scan_y";
            case TMS9918VramSlotSchedule::Activity::SpriteSelectedY: return "sprite_y";
            case TMS9918VramSlotSchedule::Activity::SpriteSelectedX: return "sprite_x";
            case TMS9918VramSlotSchedule::Activity::SpriteSelectedName: return "sprite_name";
            case TMS9918VramSlotSchedule::Activity::SpriteSelectedColour: return "sprite_colour";
            case TMS9918VramSlotSchedule::Activity::SpriteSelectedPattern: return "sprite_pattern";
            default: return "unknown";
        }
    };

    json slots = json::array();
    for (std::uint16_t i = 0; i < TMS9918SlotGrid::SlotsPerLine; ++i)
    {
        const Video::SlotActivityRecord& record = history[i];
        json entry = {
            {"slot", i},
            {"dot", TMS9918SlotGrid::SlotStartPhase(i) / 2},
            {"activity", kindName(record.activity)},
            {"index", record.index}
        };
        if (record.cpuTransacted)
        {
            entry["cpu"] = {
                {"write", record.cpuWrite},
                {"address", record.cpuAddress},
                {"value", record.cpuValue}
            };
        }
        slots.push_back(entry);
    }

    return {
        {"line", v->GetRenderLine()},
        {"current_slot", v->GetCurrentPhysicalSlot()},
        {"slots", slots}
    };
}

json DebugAdapter::SetRegion(const std::string& region)
{
    // Region decides the line count and therefore the refresh: 313 lines at
    // 50.159 Hz for PAL, 262 at 59.923 Hz for NTSC. It is a machine-level
    // choice, so it takes a reset. Stored in the emulator config as well, so a
    // ROM loaded afterwards keeps it.
    const std::string wanted = LowerAscii(region);
    Cartridge::ForceConfiguration config;
    config.type = Cartridge::CartridgeNotSupported;

    if (wanted == "pal" || wanted == "50")
    {
        config.region = Cartridge::CartridgePAL;
        config_emulator.region = 2;
    }
    else if (wanted == "ntsc" || wanted == "60")
    {
        config.region = Cartridge::CartridgeNTSC;
        config_emulator.region = 1;
    }
    else if (wanted == "auto")
    {
        config.region = Cartridge::CartridgeUnknownRegion;
        config_emulator.region = 0;
    }
    else
    {
        return {{"error", "region must be \"pal\", \"ntsc\" or \"auto\""},
                {"region", region}};
    }

    if (!m_core->GetCartridge()->IsReady())
    {
        // ResetROM is a no-op without a cartridge, so the region would silently
        // stay as it was. The choice is still recorded for the next ROM loaded.
        // This is not a failure - the setting took - so it must not carry an
        // "error" key: the transport flags isError on any response that has
        // one (see HandleToolCall), and a caller told "isError: true" after a
        // request that actually succeeded has no reason to trust the field
        // again.
        return {{"success", true},
                {"requested", wanted},
                {"note", "No cartridge inserted: the region was stored and will apply to the next ROM loaded"}};
    }

    emu_reset(config);

    const bool pal = m_core->GetVideo()->IsPAL();
    return {
        {"success", true},
        {"requested", wanted},
        {"region", pal ? "PAL" : "NTSC"},
        {"lines_per_frame", pal ? 313 : 262},
        {"refresh_hz", pal ? 50.158 : 59.923},
        {"note", "The machine was reset to apply it."}
    };
}

json DebugAdapter::SetRendererSource(const std::string& source)
{
    Video* v = m_core->GetVideo();
    const std::string wanted = LowerAscii(source);

    if (wanted == "streaming")
        v->SetPresentStreaming(true);
    else if (wanted == "legacy")
    {
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
        v->SetPresentStreaming(false);
#else
        return {{"error", "This build has the legacy renderer compiled out (GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER=0). Only \"streaming\" is available."},
                {"source", source}};
#endif
    }
    else
        return {{"error", "source must be \"legacy\" or \"streaming\""}, {"source", source}};

    return {
        {"success", true},
        {"source", v->IsPresentStreaming() ? "streaming" : "legacy"},
        {"note", "Presentation only. Both renderers keep running and being compared."}
    };
}

json DebugAdapter::ReadFrameBuffer(const std::string& source, int y, int height)
{
    Video* v = m_core->GetVideo();
    const std::string wanted = source.empty() ? std::string("presented") : LowerAscii(source);

    const u16* buffer = nullptr;
    if (wanted == "presented")
        buffer = v->GetFrameBuffer();
    else if (wanted == "legacy")
    {
#if GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER
        buffer = v->GetComposedBuffer(false);
#else
        return {{"error", "This build has the legacy renderer compiled out (GEARSF7000_ENABLE_LEGACY_VIDEO_RENDERER=0). Only \"presented\" and \"streaming\" are available."},
                {"source", source}};
#endif
    }
    else if (wanted == "streaming")
        buffer = v->GetComposedBuffer(true);
    else
        return {{"error", "source must be \"presented\", \"legacy\" or \"streaming\""}, {"source", source}};

    if (y < 0)
        y = 0;
    if (y >= GC_RESOLUTION_HEIGHT)
        return {{"error", "y is past the bottom of the frame"}, {"height", GC_RESOLUTION_HEIGHT}};
    if (height <= 0)
        height = GC_RESOLUTION_HEIGHT - y;
    if (y + height > GC_RESOLUTION_HEIGHT)
        height = GC_RESOLUTION_HEIGHT - y;

    // Both composed buffers hold TMS9918 palette indices 0..15, so one nibble
    // per pixel is lossless: a 256 pixel line becomes 64 hex characters.
    static const char kHex[] = "0123456789ABCDEF";
    json rows = json::array();
    for (int line = y; line < y + height; ++line)
    {
        std::string row;
        row.reserve(GC_RESOLUTION_WIDTH);
        const int offset = line * GC_RESOLUTION_WIDTH;
        for (int pixel = 0; pixel < GC_RESOLUTION_WIDTH; ++pixel)
            row.push_back(kHex[buffer[offset + pixel] & 0x0F]);
        rows.push_back(row);
    }

    return {
        {"source", wanted},
        {"presented_source", v->IsPresentStreaming() ? "streaming" : "legacy"},
        {"y", y},
        {"width", GC_RESOLUTION_WIDTH},
        {"height", height},
        {"encoding", "one hex nibble per pixel, TMS9918 palette index"},
        {"rows", rows}
    };
}

json DebugAdapter::GetPSGStatus(){Audio* audio=m_core->GetAudio();const Audio::PeakInfo& p=audio->GetPeakInfo();int periods[5]={};unsigned char volumes[4]={};audio->GetApu()->GetRegs(periods,volumes);json channels=json::array();const int clockHz=m_core->GetVideo()->IsPAL()?GC_MASTER_CLOCK_PAL:GC_MASTER_CLOCK_NTSC;for(int i=0;i<3;i++){const int period=periods[i];channels.push_back({{"channel",i},{"period",period},{"volume",volumes[i]},{"frequency_hz",period?static_cast<double>(clockHz)/(32.0*period):0.0}});}return {{"clock_hz",clockHz},{"tone_divider",32},{"channels",channels},{"noise",{{"control",periods[3]},{"volume",volumes[3]}}},{"peak",p.psg},{"master_peak",p.output},{"tape_peak",p.cassette}};}
json DebugAdapter::GetSF7000Status(){
#if GEARSF7000_ENABLE_SF7000
SF7000* sf=m_core->GetSF7000();if(!sf)return unavailable("SF-7000");NEC765DebugInfo f={};DiskDebugInfo d={};sf->GetFDCDebugInfo(&f);sf->GetDiskDebugInfo(&d);const uint32_t rev=sf->GetFDCBitsPerRevolution();const uint32_t pos=sf->GetFDCRotationBits();const bool motor=sf->DiskMotorOn;return {{"motor_on",motor},{"motor_requested",sf->MotorIsOn()},{"index",sf->FDCIndexActive()},{"ipl_enabled",sf->IPL_Enabled()},{"ppi2",{{"e4",sf->Port_E4},{"e5",sf->Port_E5},{"e6",sf->Port_E6},{"e7",sf->Port_E7}}},{"fdc",{{"clock_hz",sf->GetFDCClockHz()},{"main_status",f.mainStatus},{"command",f.commandCode},{"phase",f.phase},{"phase_step",f.phaseStep},{"irq",f.interrupt!=0},{"drive",f.drive},{"track",f.currentTrack},{"destination_track",f.destinationTrack},{"cylinder",f.cylinder},{"side",f.side},{"sector",f.sector},{"size_code",f.sizeCode}}},{"rotation",{{"spindle_fraction",sf->GetDiskRotationFraction()},{"raw_mfm_bits",pos},{"bits_per_revolution",rev},{"raw_mfm_fraction",rev?static_cast<double>(pos)/rev:0.0}}},{"disk",{{"present",d.present!=0},{"enabled",d.enabled!=0},{"write_protected",d.readOnly!=0},{"dirty",d.dirty!=0},{"format",static_cast<int>(d.format)},{"file",d.fileName?d.fileName:""},{"tracks",d.tracks},{"sides",d.sides},{"sectors_per_track",d.sectorsPerTrack},{"sector_size",d.sectorSize},{"rpm",d.rpm},{"bitrate_kbps",d.bitrateKbps}}}};
#else
return unavailable("SF-7000");
#endif
}
#if GEARSF7000_ENABLE_AY
json DebugAdapter::GetAY8910Status()
{
    Audio* audio = m_core->GetAudio();
    if (!audio->IsAyEnabled())
        return {{"present", false}, {"reason", "expansion disabled; enable it and reset"}};

    Ay_Apu* ay = audio->GetAyApu();
    const int clockHz = audio->GetAyClockHz();

    u8 r[16];
    json regs = json::array();
    for (int i = 0; i < 16; ++i) { r[i] = static_cast<u8>(ay->read(i)); regs.push_back(r[i]); }

    // The audible tone is clock/(32*period): the datasheet's /16 counts half
    // oscillations. Same convention as get_psg_status reports for the PSG.
    json channels = json::array();
    for (int i = 0; i < 3; ++i)
    {
        const int period = ((r[i * 2 + 1] & 0x0F) << 8) | r[i * 2];
        channels.push_back({
            {"channel", i},
            {"period", period},
            {"frequency_hz", period ? static_cast<double>(clockHz) / (32.0 * period) : 0.0},
            {"volume", r[8 + i] & 0x0F},
            {"envelope_mode", (r[8 + i] & 0x10) != 0},
            {"tone_enabled", (r[7] & (1 << i)) == 0},
            {"noise_enabled", (r[7] & (1 << (i + 3))) == 0}});
    }

    const int envPeriod = (r[12] << 8) | r[11];
    return {
        {"present", true},
        {"chip", audio->GetAyChip() == Audio::AyChip::YM2149 ? "YM2149" : "AY-3-8910"},
        {"clock_hz", clockHz},
        {"tone_divider", 32},
        {"port_base", GetAyPortBase()},
        {"selected_register", audio->GetAySelectedRegister()},
        {"channels", channels},
        {"noise", {{"period", r[6] & 0x1F}}},
        {"envelope", {{"period", envPeriod},
                      {"shape", r[13] & 0x0F},
                      {"steps", audio->GetAyChip() == Audio::AyChip::YM2149 ? 32 : 16}}},
        {"registers", regs}};
}
#else
json DebugAdapter::GetAY8910Status(){return unavailable("AY-3-8910");}
#endif

json DebugAdapter::GetScreenshot()
{
    // A cartridge is not the only way this machine is ready to present a
    // frame: SF-7000 IPL+RAM alone boot a complete machine with no SC-3000
    // cartridge inserted at all (see GearSF7000Core::IsMachineReady()).
    if (!m_core->IsMachineReady())
        return {{"error", "No cartridge or SF-7000 IPL loaded"}};

    unsigned char* pngBuffer = nullptr;
    const int pngSize = emu_get_screenshot_png(&pngBuffer);
    if (pngSize == 0 || pngBuffer == nullptr)
        return {{"error", "Unable to capture the frame buffer"}};

    const std::string base64Png = Base64Encode(pngBuffer, pngSize);
    free(pngBuffer);

    GC_RuntimeInfo runtime;
    emu_get_runtime(runtime);

    // width/height are the packed frame, border included when overscan is
    // on - the same mismatch this session found in the Sprites debug view's
    // on-screen marker, which forgot this and drew every sprite offset by
    // exactly the border's thickness. content_area is where the real
    // 256x192 picture starts inside that frame, so a caller can place a SAT
    // X/Y (always active-area-relative) onto this image without repeating
    // that mistake.
    const GC_VideoFrameDescriptor descriptor = m_core->GetVideoFrameDescriptor();

    return {
        {"__mcp_image", true},
        {"data", base64Png},
        {"mimeType", "image/png"},
        {"width", runtime.screen_width},
        {"height", runtime.screen_height},
        {"content_area", {
            {"x", descriptor.content_area.x},
            {"y", descriptor.content_area.y},
            {"width", descriptor.content_area.width},
            {"height", descriptor.content_area.height}
        }}
    };
}

json DebugAdapter::SetFullRasterDebugEnabled(bool enabled)
{
    emu_set_full_raster_debug_enabled(enabled);
    return {{"success", true}, {"enabled", enabled}};
}

json DebugAdapter::DumpCrtSignal(const std::string& file_path, int lines)
{
    if (!m_core->IsMachineReady())
        return {{"error", "No cartridge or SF-7000 IPL loaded"}};
    if (file_path.empty())
        return {{"error", "file_path is required"}};
    if (lines <= 0)
        return {{"error", "lines must be positive"}};
    if (emu_crt_signal_dump_in_progress())
        return {{"error", "A CRT signal dump is already running"}};
    if (!emu_crt_signal_dump_start(file_path.c_str(), lines))
        return {{"error", "Could not start the CRT signal dump"}};

    // Capture spans frames; the caller polls this same tool's
    // "in_progress" report or simply waits before reading the file.
    return {
        {"success", true},
        {"file_path", file_path},
        {"lines", lines},
        {"in_progress", true}
    };
}

json DebugAdapter::ExportMemoryToFile(int area, u32 offset, size_t length, const std::string& file_path)
{
    if (!m_core->IsMachineReady())
        return {{"error", "No cartridge or SF-7000 IPL loaded"}};
    if (file_path.empty())
        return {{"error", "file_path is required"}};
    if (length == 0)
        return {{"error", "length must be positive"}};

    std::vector<u8> data = ReadMemoryArea(area, offset, length);
    if (data.empty())
        return {{"error", "Nothing read - check area and offset are valid (see list_memory_areas)"}};

    FILE* file = fopen(file_path.c_str(), "wb");
    if (!file)
        return {{"error", "Could not open file for writing: " + file_path}};
    size_t written = fwrite(data.data(), 1, data.size(), file);
    fclose(file);
    if (written != data.size())
        return {{"error", "Short write to file"}};

    return {
        {"success", true},
        {"file_path", file_path},
        {"area", area},
        {"offset", offset},
        {"bytes_written", written}
    };
}

json DebugAdapter::ImportMemoryFromFile(int area, u32 offset, const std::string& file_path)
{
    if (!m_core->IsMachineReady())
        return {{"error", "No cartridge or SF-7000 IPL loaded"}};
    if (file_path.empty())
        return {{"error", "file_path is required"}};

    FILE* file = fopen(file_path.c_str(), "rb");
    if (!file)
        return {{"error", "Could not open file for reading: " + file_path}};
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (file_size <= 0)
    {
        fclose(file);
        return {{"error", "File is empty or unreadable"}};
    }

    std::vector<u8> data(static_cast<size_t>(file_size));
    size_t bytes_read = fread(data.data(), 1, data.size(), file);
    fclose(file);
    data.resize(bytes_read);

    WriteMemoryArea(area, offset, data);

    return {
        {"success", true},
        {"file_path", file_path},
        {"area", area},
        {"offset", offset},
        {"bytes_imported", data.size()}
    };
}

json DebugAdapter::SaveDisassemblyFile(bool full, const std::string& file_path)
{
    if (!m_core->IsMachineReady())
        return {{"error", "No cartridge or SF-7000 IPL loaded"}};
    if (file_path.empty())
        return {{"error", "file_path is required"}};

    if (!gui_debug_save_disassembly(file_path.c_str(), full))
        return {{"error", "Could not open file for writing: " + file_path}};

    return {
        {"success", true},
        {"file_path", file_path},
        {"full", full}
    };
}

json DebugAdapter::SaveDebugPng(const std::string& view, int sprite_index, const std::string& file_path)
{
    if (!m_core->IsMachineReady())
        return {{"error", "No cartridge or SF-7000 IPL loaded"}};
    if (file_path.empty())
        return {{"error", "file_path is required"}};

    if (view == "tiles")
    {
        // lines is always 32 in the GUI view, matching the buffer's full
        // 256x256 allocation exactly - no crop needed.
        emu_save_debug_png(file_path.c_str(), emu_debug_tile_buffer, 256, 256, 256 * 3);
        return {{"success", true}, {"file_path", file_path}, {"view", view}, {"width", 256}, {"height", 256}};
    }
    else if (view == "background")
    {
        Video* video = m_core->GetVideo();
        int mode = video->GetMode();
        int cols = (mode == 1) ? 40 : 32;
        int rows = 24;
        int crop_width = (mode == 1) ? (int)(6.0f * 40.0f) : 256;
        int crop_height = rows * 8;
        emu_save_debug_png(file_path.c_str(), emu_debug_background_buffer, crop_width, crop_height, 256 * 3);
        return {{"success", true}, {"file_path", file_path}, {"view", view}, {"width", crop_width}, {"height", crop_height}, {"cols", cols}};
    }
    else if (view == "sprite")
    {
        if ((sprite_index < 0) || (sprite_index >= 32))
            return {{"error", "sprite_index must be 0-31"}};

        Video* video = m_core->GetVideo();
        u8* regs = video->GetRegisters();
        bool sprites_16 = IsSetBit(regs[1], 1);
        int sprite_size = sprites_16 ? 16 : 8;
        emu_save_debug_png(file_path.c_str(), emu_debug_sprite_buffers[sprite_index], sprite_size, sprite_size, 16 * 3);
        return {{"success", true}, {"file_path", file_path}, {"view", view}, {"sprite_index", sprite_index}, {"width", sprite_size}, {"height", sprite_size}};
    }

    return {{"error", "view must be \"background\", \"tiles\", or \"sprite\""}};
}

json DebugAdapter::GetFullRasterDebugScreenshot()
{
    if (!m_core->IsMachineReady())
        return {{"error", "No cartridge or SF-7000 IPL loaded"}};

    unsigned char* pngBuffer = nullptr;
    const int pngSize = emu_get_full_raster_debug_png(&pngBuffer);
    if (pngSize == 0 || pngBuffer == nullptr)
        return {{"error", "Full-raster debug is disabled - call set_full_raster_debug_enabled first"}};

    const std::string base64Png = Base64Encode(pngBuffer, pngSize);
    free(pngBuffer);

    return {
        {"__mcp_image", true},
        {"data", base64Png},
        {"mimeType", "image/png"},
        {"width", TMS9918RasterTiming::DotsPerLine},
        {"height", m_core->GetVideo()->IsPAL() ? GC_LINES_PER_FRAME_PAL : GC_LINES_PER_FRAME_NTSC}
    };
}


json DebugAdapter::ListSprites(int line, const std::string& source)
{
    Video* video = m_core->GetVideo();
    if (video == NULL)
        return unavailable("Sprite inspection");

    const Video::SpriteDebugSnapshot snap = video->GetSpriteDebugSnapshot(line);

    if (source == "rendered")
    {
        const Video::RenderedSpriteDebugFrame& frame =
            video->GetLastRenderedSpriteDebugFrame();
        if (!frame.valid)
            return {{"error", "No complete rendered frame has been captured"}};

        const int height = frame.large ? 16 : 8;
        const u16 completeMask = frame.large ? 0xFFFFu : 0x00FFu;
        json list = json::array();
        for (int index = 0; index < 32; ++index)
        {
            const Video::RenderedSpriteDebugEntry& e = frame.entries[index];
            const u16 rows = e.fetchedRows & completeMask;
            const char* result = !e.seen ? "not_fetched"
                : (rows == completeMask ? "complete" : "partial");
            char rowsHex[5];
            snprintf(rowsHex, sizeof(rowsHex), "%04X", rows);
            list.push_back({
                {"index", index},
                {"seen", e.seen},
                {"raw_y", e.rawY},
                {"visible_y", (e.rawY + 1) & 0xFF},
                {"x", e.x},
                {"effective_x", static_cast<int>(e.x) -
                    ((e.colour & 0x80) ? 32 : 0)},
                {"early_clock", (e.colour & 0x80) != 0},
                {"pattern", e.name},
                {"colour", e.colour & 0x0F},
                {"fetched_rows", rows},
                {"fetched_rows_hex", rowsHex},
                {"result", result}
            });
        }
        return {
            {"source", "rendered"},
            {"frame_serial", frame.frameSerial},
            {"sprites", list},
            {"count", 32},
            {"sprites_enabled", frame.spritesEnabled},
            {"height", height},
            {"width", height},
            {"magnified", frame.magnified},
            {"legend", {
                {"complete", "Every source row was fetched at least once."},
                {"partial", "Only some rows were fetched (clipping, raster changes, or four-sprite overflow)."},
                {"not_fetched", "This SAT index supplied no row to the completed frame."}
            }},
            {"last_frame", {
                {"collisions", snap.collisions},
                {"collision_line", snap.collisionLine},
                {"collision_dot", snap.collisionDot},
                {"overflow_line", snap.overflowLine},
                {"overflow_sprite", snap.overflowSprite}
            }}
        };
    }
    if (source != "live")
        return {{"error", "source must be live or rendered"}};

    json list = json::array();
    for (int i = 0; i < snap.count; ++i)
    {
        const Video::SpriteDebugEntry& e = snap.entries[i];
        json entry = {
            {"index", e.index},
            // Both, always. raw_y is the byte in the table; visible_y is the
            // line it draws on, which is one lower - the off-by-one that
            // makes every comparison against the screen wrong if only one of
            // the two is reported.
            {"raw_y", e.rawY},
            {"visible_y", e.visibleY},
            {"x", e.x},
            {"effective_x", e.effectiveX},
            {"early_clock", e.earlyClock},
            {"pattern", e.name},
            {"colour", e.colour & 0x0F}
        };
        // Only when the answer means something. The chip keeps selection
        // state for the line it is on, so these are absent rather than false
        // when the snapshot could not be tied to that line - false would read
        // as "not selected", which is a different claim.
        if (snap.lineStateValid)
        {
            entry["selected_on_line"] = e.selectedOnLine;
            entry["overflow_cause"] = e.overflowCause;
        }
        list.push_back(entry);
    }

    json result = {
        {"source", "live"},
        {"sprites", list},
        {"count", snap.count},
        {"line", snap.line},
        {"line_state_valid", snap.lineStateValid},
        {"sprites_enabled", snap.spritesEnabled},
        {"height", snap.height},
        {"magnified", snap.magnified},
        {"width", snap.height},
        // The last COMPLETE frame, not the one in progress - a debugger is
        // usually paused in the VBlank, where the live counters have already
        // reset and would report nothing.
        //
        // Frame-level, not per-sprite: the hardware records that a collision
        // happened, not which pair caused it, so there is no per-sprite flag
        // to report and none is invented here.
        {"last_frame", {
            {"collisions", snap.collisions},
            {"collision_line", snap.collisionLine},
            {"collision_dot", snap.collisionDot},
            {"overflow_line", snap.overflowLine},
            {"overflow_sprite", snap.overflowSprite}
        }}
    };

    if (snap.terminatorIndex >= 0)
        result["terminator_index"] = snap.terminatorIndex;
    else
        result["note"] = "No $D0 terminator in the table: all 32 entries are "
                         "part of the list.";

    if (!snap.lineStateValid)
        result["line_note"] =
            "Selection state is only kept for the line the beam is on, so "
            "selected_on_line and overflow_cause are omitted. Pause on the "
            "line of interest to get them.";

    if (!snap.spritesEnabled)
        result["mode_note"] =
            "The current mode has no sprite plane, so nothing in this table "
            "is drawn.";

    return result;
}

json DebugAdapter::GetSpritePipeline()
{
    Video* video = m_core->GetVideo();
    if (video == NULL)
        return unavailable("Sprite pipeline inspection");
    const Video::SpritePipelineDebugSnapshot snap =
        video->GetSpritePipelineDebugSnapshot();
    const auto activityName = [](TMS9918VramSlotSchedule::Activity activity) {
        switch (activity)
        {
            case TMS9918VramSlotSchedule::Activity::SpriteScanY: return "sprite_scan_y";
            case TMS9918VramSlotSchedule::Activity::SpriteSelectedY: return "sprite_y";
            case TMS9918VramSlotSchedule::Activity::SpriteSelectedX: return "sprite_x";
            case TMS9918VramSlotSchedule::Activity::SpriteSelectedName: return "sprite_name";
            case TMS9918VramSlotSchedule::Activity::SpriteSelectedColour: return "sprite_colour";
            case TMS9918VramSlotSchedule::Activity::SpriteSelectedPattern: return "sprite_pattern";
            case TMS9918VramSlotSchedule::Activity::Cpu: return "cpu";
            case TMS9918VramSlotSchedule::Activity::Refresh: return "refresh";
            case TMS9918VramSlotSchedule::Activity::Name: return "name";
            case TMS9918VramSlotSchedule::Activity::Colour: return "colour";
            case TMS9918VramSlotSchedule::Activity::Pattern: return "pattern";
            default: return "unknown";
        }
    };

    json lines = json::array();
    for (const Video::SpritePipelineLineDebug& line : snap.lines)
    {
        if (!line.valid)
            continue;
        json selected = json::array();
        for (int slot = 0; slot < line.selectedCount && slot < 4; ++slot)
        {
            const Video::SpritePipelineLatchDebug& e = line.selected[slot];
            selected.push_back({
                {"slot", slot}, {"sat_index", e.satIndex},
                {"raster_x", e.rasterX},
                {"raster_dot", e.rasterX == 0xFF ? -1 :
                    static_cast<int>(TMS9918SlotGrid::SlotStartPhase(
                        e.rasterX) / 2)},
                {"sat", {{"y", e.y}, {"x", e.x}, {"name", e.name},
                         {"colour", e.colour}}},
                {"valid", {{"y", e.yValid}, {"x", e.xValid},
                           {"name", e.nameValid}, {"colour", e.colourValid},
                           {"pattern_0", e.patternValid[0]},
                           {"pattern_1", e.patternValid[1]}}},
                {"pattern_bytes", {e.pattern[0], e.pattern[1]}}
            });
        }
        lines.push_back({
            {"absolute_target_line", line.absoluteTargetLine},
            {"target_line", line.targetLine},
            {"selected_count", line.selectedCount},
            {"selection_terminated", line.selectionTerminated},
            {"overflow", line.overflowRecorded},
            {"overflow_sat_index", line.overflowRecorded
                ? line.overflowSprite : -1},
            {"overflow_raster_x", line.overflowRecorded
                ? static_cast<int>(line.overflowRasterX) : -1},
            {"overflow_sat", line.overflowSatValid
                ? json{{"y", line.overflowSat[0]},
                       {"x", line.overflowSat[1]},
                       {"name", line.overflowSat[2]},
                       {"colour", line.overflowSat[3]}}
                : json(nullptr)},
            {"overflow_sat_note", line.overflowRecorded
                ? "Only Y was fetched; X/name/colour are a same-instant diagnostic snapshot."
                : ""},
            {"collision_x", line.collisionX},
            {"selected", selected}
        });
    }

    json result = {
        {"raster_line", snap.rasterLine},
        {"absolute_raster_line", snap.absoluteRasterLine},
        {"phase", snap.phase}, {"dot", snap.dot}, {"slot", snap.slot},
        {"activity", activityName(snap.activity)},
        {"activity_index", snap.activityIndex},
        {"activity_byte", snap.activityByte},
        {"request_pending", snap.requestPending}, {"lines", lines}
    };
    if (snap.activeTargetValid)
    {
        result["active_target_line"] = snap.activeTargetLine;
        result["active_absolute_target_line"] =
            snap.activeAbsoluteTargetLine;
    }
    return result;
}

json DebugAdapter::GetSpriteScanlineHistory(int startLine, int endLine,
                                            bool includeEmpty)
{
    Video* video = m_core->GetVideo();
    if (video == NULL)
        return unavailable("Sprite scanline history");
    const Video::SpriteSelectionFrameHistory& history =
        video->GetSpriteSelectionFrameHistory();
    if (!history.valid)
        return {{"error", "No completed or recorded sprite frame history"}};

    const int rasterLines = history.rasterLines > 0
        ? history.rasterLines : GC_LINES_PER_FRAME_NTSC;
    startLine = std::max(0, startLine);
    if (endLine < 0)
        endLine = rasterLines - 1;
    if (startLine >= rasterLines || endLine < startLine || endLine >= rasterLines)
        return {{"error", "scanline range is outside the recorded frame"},
                {"raster_lines", rasterLines}};

    json rows = json::array();
    for (int y = startLine; y <= endLine; ++y)
    {
        const Video::SpriteSelectionHistoryRow& row = history.rows[y];
        bool hasSprite = false;
        json slots = json::array();
        for (int lane = 0; lane < 5; ++lane)
        {
            if (row.rasterX[lane] == 0xFF)
                continue;
            hasSprite = true;
            const auto scan = TMS9918VramSlotSchedule::GetSlot(
                TMS9918VramSlotSchedule::Schedule::Graphics,
                row.rasterX[lane]);
            json item = {
                {"lane", lane}, {"overflow_candidate", lane == 4},
                {"raster_x", row.rasterX[lane]},
                {"raster_dot", TMS9918SlotGrid::SlotStartPhase(
                    row.rasterX[lane]) / 2},
                {"sat_index", scan.activity ==
                    TMS9918VramSlotSchedule::Activity::SpriteScanY
                        ? static_cast<int>(scan.index) : -1},
                {"sat", {{"y", row.sat[lane][0]},
                         {"x", row.sat[lane][1]},
                         {"name", row.sat[lane][2]},
                         {"colour", row.sat[lane][3]}}}
            };
            if (lane < 4)
                item["fetched_pattern_bytes"] = {
                    row.pattern[lane][0], row.pattern[lane][1]};
            else
                item["hardware_read_note"] =
                    "Only Y was fetched; the remaining SAT bytes are a same-instant diagnostic snapshot.";
            slots.push_back(item);
        }
        if (!includeEmpty && !hasSprite && (row.flags & 0x30) == 0)
            continue;
        rows.push_back({
            {"line", y}, {"valid", (row.flags & 0x01) != 0},
            {"sprites_enabled", (row.flags & 0x02) != 0},
            {"large_16x16", (row.flags & 0x04) != 0},
            {"magnified", (row.flags & 0x08) != 0},
            {"overflow", (row.flags & 0x10) != 0},
            {"collision", (row.flags & 0x20) != 0},
            {"collision_x", (row.flags & 0x20) != 0
                ? static_cast<int>(row.collisionX) : -1},
            {"slots", slots}
        });
    }
    return {
        {"frame_serial", history.frameSerial},
        {"raster_lines", rasterLines},
        {"start_line", startLine}, {"end_line", endLine},
        {"rows", rows}, {"returned_rows", rows.size()},
        {"record_bytes_per_line", 35}, {"empty_value", 255}
    };
}

json DebugAdapter::GetSpriteImage(int index, const std::string& source)
{
    if (index < 0 || index > 31)
        return {{"error", "sprite_index must be 0-31"}, {"sprite_index", index}};

    Video* video = m_core->GetVideo();
    if (video == NULL)
        return unavailable("Sprite image");

    if (source == "rendered")
    {
        const Video::RenderedSpriteDebugFrame& frame =
            video->GetLastRenderedSpriteDebugFrame();
        if (!frame.valid)
            return {{"error", "No complete rendered frame has been captured"}};

        const int side = frame.large ? 16 : 8;
        const Video::RenderedSpriteDebugEntry& entry = frame.entries[index];
        const u8* palette = video->GetCurrentPalette();
        std::vector<u8> rgba(static_cast<std::size_t>(side) * side * 4, 0);
        for (int y = 0; y < side; ++y)
            for (int x = 0; x < side; ++x)
            {
                const int colour = entry.pixels[y * 16 + x] & 0x0F;
                if (colour == 0)
                    continue;
                const std::size_t p =
                    (static_cast<std::size_t>(y) * side + x) * 4;
                rgba[p + 0] = palette[colour * 3 + 0];
                rgba[p + 1] = palette[colour * 3 + 1];
                rgba[p + 2] = palette[colour * 3 + 2];
                rgba[p + 3] = 255;
            }

        unsigned char* png = NULL;
        const int pngSize =
            emu_encode_png_rgba(rgba.data(), side, side, &png);
        if (png == NULL || pngSize == 0)
            return {{"error", "Could not encode the rendered sprite as PNG"}};
        const std::string base64 = Base64Encode(png, pngSize);
        free(png);
        return {
            {"__mcp_image", true},
            {"data", base64},
            {"mimeType", "image/png"},
            {"width", side},
            {"height", side},
            {"sprite_index", index},
            {"source", "rendered"},
            {"frame_serial", frame.frameSerial},
            {"seen", entry.seen},
            {"fetched_rows", entry.fetchedRows}
        };
    }
    if (source != "live")
        return {{"error", "source must be live or rendered"}};

    const Video::SpriteDebugSnapshot snap = video->GetSpriteDebugSnapshot(-1);

    // Past the terminator there is no sprite - the chip never looks there.
    // Returning a picture of whatever bytes happen to be in VRAM would be
    // inventing an object.
    if (snap.terminatorIndex >= 0 && index >= snap.terminatorIndex)
        return {{"error", "Past the $D0 terminator: this entry is not part of "
                          "the sprite list"},
                {"sprite_index", index},
                {"terminator_index", snap.terminatorIndex}};

    const Video::SpriteDebugEntry* entry = NULL;
    for (int i = 0; i < snap.count; ++i)
        if (snap.entries[i].index == index)
            entry = &snap.entries[i];
    if (entry == NULL)
        return {{"error", "No such sprite"}, {"sprite_index", index}};

    const u8* vram = video->GetVRAM();
    const u8* regs = video->GetRegisters();
    const u16 patternBase = static_cast<u16>((regs[6] & 0x07) << 11);

    const int side = snap.height;               // 8 or 16, before magnification
    // In 16x16 mode the pattern number's low two bits are ignored: one sprite
    // is four consecutive 8x8 patterns, quadrants in column order.
    const int name = (side == 16) ? (entry->name & 0xFC) : entry->name;

    const u8* palette = video->GetCurrentPalette();
    const int colourIndex = entry->colour & 0x0F;
    const u8 r = palette[colourIndex * 3 + 0];
    const u8 g = palette[colourIndex * 3 + 1];
    const u8 b = palette[colourIndex * 3 + 2];

    std::vector<u8> rgba(static_cast<std::size_t>(side) * side * 4, 0);
    for (int y = 0; y < side; ++y)
    {
        for (int x = 0; x < side; ++x)
        {
            // Quadrant order for 16x16: +0 top-left, +1 bottom-left,
            // +2 top-right, +3 bottom-right - down the column first, which is
            // the order that catches people out.
            int patternOffset = 0;
            int localY = y;
            if (side == 16)
            {
                patternOffset = ((x >= 8) ? 2 : 0) + ((y >= 8) ? 1 : 0);
                localY = y & 7;
            }
            const u16 addr = static_cast<u16>(
                (patternBase + ((name + patternOffset) << 3) + localY) & 0x3FFF);
            const u8 row = vram[addr];
            const bool on = (row & (0x80 >> (x & 7))) != 0;

            const std::size_t p = (static_cast<std::size_t>(y) * side + x) * 4;
            if (on)
            {
                rgba[p + 0] = r;
                rgba[p + 1] = g;
                rgba[p + 2] = b;
                rgba[p + 3] = 255;
            }
            // else left at 0,0,0,0 - transparent. A sprite pixel that is off
            // shows the background through, so the PNG has to say so rather
            // than painting it black.
        }
    }

    unsigned char* png = NULL;
    const int pngSize = emu_encode_png_rgba(rgba.data(), side, side, &png);
    if (png == NULL || pngSize == 0)
        return {{"error", "Could not encode the sprite as PNG"}};

    const std::string base64 = Base64Encode(png, pngSize);
    free(png);

    return {
        {"__mcp_image", true},
        {"data", base64},
        {"mimeType", "image/png"},
        {"width", side},
        {"height", side},
        {"sprite_index", index},
        {"source", "live"},
        {"pattern", name},
        {"colour", colourIndex},
        // Deliberately not applied and deliberately said: the TMS9918 has no
        // flip bit. Bit 7 of the colour byte is early clock. Flipping out of
        // habit from other hardware produces a wrong picture.
        {"note", "No flip is applied: the TMS9918 has none. Colour 0 pixels "
                 "are transparent, not black."}
    };
}
json DebugAdapter::GetMediaInfo(){Cartridge* c=m_core->GetCartridge();json result={{"rom",c->GetFilePath()},{"rom_ready",c->IsReady()},{"sf7000_enabled",m_core->GetMemory()->IsSF7000Enabled()}};
#if GEARSF7000_ENABLE_SR1000
SR1000* t=m_core->GetCassette();result["cassette_loaded"]=t->IsLoaded();result["cassette_playing"]=t->IsPlaying();result["cassette_motor"]=t->IsMotorOn();result["cassette_motor_requested"]=t->IsMotorRequested();result["cassette_position_percent"]=t->GetPositionPercentage();result["cassette_counter"]=t->GetCounter();result["cassette_counter_max"]=t->GetCounterMax();result["cassette_speed_target_percent"]=t->GetTapeSpeedPercent();result["cassette_speed_effective_percent"]=t->GetEffectiveTapeSpeedPercent();result["cassette_info"]=t->GetInfo();
#endif
return result;}
json DebugAdapter::ListRecentRoms(){json roms=json::array();for(int i=0;i<config_max_recent_roms;i++)if(!config_emulator.recent_roms[i].empty())roms.push_back({{"index",i},{"file_path",config_emulator.recent_roms[i]}});return {{"roms",roms}};}
json DebugAdapter::ListRecentTapes(){json tapes=json::array();for(int i=0;i<config_max_recent_cassettes;i++)if(!config_emulator.recent_cassettes[i].empty())tapes.push_back({{"index",i},{"file_path",config_emulator.recent_cassettes[i]}});return {{"tapes",tapes}};}
json DebugAdapter::ListRecentDisks(){json disks=json::array();for(int i=0;i<config_max_recent_discs;i++)if(!config_emulator.recent_discs[i].empty())disks.push_back({{"index",i},{"file_path",config_emulator.recent_discs[i]}});return {{"disks",disks}};}
json DebugAdapter::LoadRom(const std::string& file_path){const std::filesystem::path romPath(file_path);if(file_path.empty()||!romPath.is_absolute())return {{"error","file_path must be an absolute local path"}};std::error_code pathError;if(!std::filesystem::is_regular_file(romPath,pathError))return {{"error","ROM path does not identify a readable local file"},{"file_path",file_path}};const bool loaded=application_load_rom(file_path.c_str());if(!loaded)return {{"error","Unable to load ROM; current ROM was left unchanged"},{"file_path",file_path}};std::filesystem::path symbolPath=romPath;symbolPath.replace_extension(".sym");json result={{"success",true},{"file_path",file_path},{"symbol_path",symbolPath.string()},{"symbol_file_present",std::filesystem::is_regular_file(symbolPath,pathError)},{"paused",m_core->IsPaused()}};
    // Loading a cartridge throws things away. Saying so beats letting a client
    // discover later that its breakpoints are gone: Memory::ResetRomDisassembledMemory()
    // clears the CPU breakpoint list along with the disassembly it points into,
    // and the recording belonged to the outgoing machine.
    result["discarded"]={{"cpu_breakpoints",true},{"rewind_history",true},{"pending_frame_step",true}};
    result["machine_generation"]=emu_get_machine_generation();
    return result;}
json DebugAdapter::EjectRom()
{
    if (!m_core->GetCartridge()->IsReady())
        return {{"error", "No cartridge inserted"}};

    emu_eject_rom();
    return {
        {"success", true},
        {"rom_ready", m_core->GetCartridge()->IsReady()},
        {"note", "The machine is back to the state it is in before anything is loaded."}
    };
}

json DebugAdapter::SetStartPaused(bool paused)
{
    // The same switch the GUI's File > Start Paused checkbox drives.
    // load_rom/start_sf7000 already check it right after loading, before any
    // instruction has executed, so the effect is a breakpoint at 0000 armed
    // ahead of time and debug_continue starting the ROM from reset with it
    // live, rather than racing whatever runs first.
    config_emulator.start_paused = paused;
    return {
        {"success", true},
        {"start_paused", config_emulator.start_paused},
        {"note", "Applies to the next load_rom/start_sf7000, not to what is already running."}
    };
}
json DebugAdapter::LoadTape(const std::string& file_path){const std::filesystem::path tapePath(file_path);if(file_path.empty()||!tapePath.is_absolute())return {{"error","file_path must be an absolute local path"}};std::error_code pathError;if(!std::filesystem::is_regular_file(tapePath,pathError))return {{"error","Tape path does not identify a readable local file"},{"file_path",file_path}};if(!application_load_tape(file_path.c_str()))return {{"error","Unable to load tape; current tape was left unchanged"},{"file_path",file_path}};return {{"success",true},{"file_path",file_path},{"write_protected",config_emulator.cassette_write_protected}};}
json DebugAdapter::SetTapeSpeed(float percent){
#if GEARSF7000_ENABLE_SR1000
if(percent<-20.0f||percent>20.0f)return {{"error","speed must be between -20.0 and +20.0"}};m_core->GetCassette()->SetTapeSpeedPercent(percent);return {{"success",true},{"cassette_speed_target_percent",percent}};
#else
(void)percent;return unavailable("SR-1000");
#endif
}
json DebugAdapter::TapePlay(){
#if GEARSF7000_ENABLE_SR1000
SR1000* t=m_core->GetCassette();if(!t->IsLoaded())return {{"error","No tape loaded"}};emu_cassette_play();return {{"success",true},{"playing",true}};
#else
return unavailable("SR-1000");
#endif
}
json DebugAdapter::TapeStop(){emu_cassette_stop();return {{"success",true},{"playing",false}};}
json DebugAdapter::TapeRewind(){emu_cassette_rewind();return {{"success",true},{"position_percent",0.0}};}
json DebugAdapter::MountDisk(const std::string& file_path,bool write_protected){const std::filesystem::path diskPath(file_path);if(file_path.empty()||!diskPath.is_absolute())return {{"error","file_path must be an absolute local path"}};std::error_code pathError;if(!std::filesystem::is_regular_file(diskPath,pathError))return {{"error","Disk path does not identify a readable local file"},{"file_path",file_path}};if(!application_mount_disk(file_path.c_str(),write_protected))return {{"error","Unable to mount disk; current disk was left unchanged"},{"file_path",file_path}};return {{"success",true},{"file_path",file_path},{"write_protected",config_emulator.disc_write_protected},{"sf7000_started",false}};}
json DebugAdapter::StartSF7000(){if(!application_start_sf7000())return {{"error","Unable to start SF-7000. Configure a valid 8 KB IPL path in SF-7000 > IPL first."},{"ipl_path",config_emulator.bios_path}};return {{"success",true},{"ipl_path",config_emulator.bios_path},{"ipl_loaded",emu_is_bios_loaded()},{"paused",m_core->IsPaused()}};}
json DebugAdapter::ListSaveStateSlots(){return {{"slots",json::array({0,1,2,3,4,5,6,7,8,9})}};} json DebugAdapter::SelectSaveStateSlot(int){return unavailable("Selected save-state slot");} 
// These four used to answer {"success":true} without looking at what the core
// did, which is how a zero-byte file could be reported as a completed save.
// Every one of them now reports what actually happened.
json DebugAdapter::SaveState()
{
    const bool ok = emu_save_state_slot(0);
    if (!ok)
        return {{"success", false}, {"slot", 0},
                {"error", "save_state_failed"},
                {"reason", m_core->GetLastStateError()}};
    return {{"success", true}, {"slot", 0},
            {"format_version", GearSF7000Core::GetSaveStateFormatVersion()}};
}

json DebugAdapter::LoadState()
{
    const bool ok = emu_load_state_slot(0);
    if (!ok)
        return {{"success", false}, {"slot", 0},
                {"error", "load_state_failed"},
                {"reason", m_core->GetLastStateError()}};

    // Landed paused, like rewind_seek: reading the machine straight after a
    // load has to describe the state that was loaded, not one the emulator
    // has already run past.
    const bool hadHistory = rewind_is_enabled();
    emu_pause();
    return {{"success", true}, {"slot", 0}, {"paused", true},
            {"rewind_history_reset", hadHistory}};
}

json DebugAdapter::SaveStateFile(const std::string& p)
{
    size_t bytesWritten = 0;
    const bool ok = emu_save_state_file(p.c_str(), &bytesWritten);
    if (!ok)
        return {{"success", false}, {"file_path", p},
                {"error", "save_state_failed"},
                {"reason", m_core->GetLastStateError()}};

    json result = {{"success", true},
                   {"file_path", p},
                   {"bytes_written", bytesWritten},
                   {"format_version",
                    GearSF7000Core::GetSaveStateFormatVersion()}};
    if (Video* video = m_core->GetVideo())
        result["frame_serial"] =
            video->GetFrameRenderDiagnostics().frameSerial;
    return result;
}

json DebugAdapter::LoadStateFile(const std::string& p)
{
    const bool ok = emu_load_state_file(p.c_str());
    if (!ok)
        return {{"success", false}, {"file_path", p},
                {"error", "load_state_failed"},
                {"reason", m_core->GetLastStateError()}};

    // Landed paused, like rewind_seek: reading the machine straight after a
    // load has to describe the state that was loaded, not one the emulator
    // has already run past.
    const bool hadHistory = rewind_is_enabled();
    emu_pause();

    json result = {{"success", true}, {"file_path", p}, {"paused", true}};
    if (Video* video = m_core->GetVideo())
        result["frame_serial"] =
            video->GetFrameRenderDiagnostics().frameSerial;

    // A state from outside the recorder describes a different line of play,
    // so the recording that was running has been abandoned. Say so rather
    // than leaving a client to discover that the timeline emptied.
    result["rewind_history_reset"] = hadHistory;
    return result;
}
 json DebugAdapter::SetFastForwardSpeed(int speed)
{
    emu_set_fast_forward_speed(speed);
    return {{"success", true},
            {"fast_forward", emu_get_fast_forward()},
            {"speed_setting", emu_get_fast_forward_speed()},
            {"note", "0=1.5x 1=2x 2=2.5x 3=3x 4=unlimited; a ceiling, not a guarantee - read get_rewind_status.speed_ratio for what is actually achieved."}};
}

json DebugAdapter::ToggleFastForward(bool enabled)
{
    emu_set_fast_forward(enabled);
    return {{"success", true},
            {"fast_forward", emu_get_fast_forward()},
            {"speed_setting", emu_get_fast_forward_speed()},
            {"pacing", config_video_pacing_name()},
            };
} 
json DebugAdapter::GetRewindStatus()
{
    const RewindStatus s = rewind_get_status();

    json result = {
        {"available", s.available},
        {"enabled", s.enabled},
        {"format_version", s.format_version},
        {"snapshot_count", s.snapshot_count},
        {"capacity", s.capacity},
        {"frames_per_snapshot", s.frames_per_snapshot},
        {"buffer_seconds", s.buffer_seconds},
        {"buffered_seconds", s.buffered_seconds},
        {"max_age_seconds", s.max_age_seconds},
        {"frames_per_second", s.frames_per_second},
        {"wall_clock_fps", s.wall_clock_fps},
        {"speed_ratio", s.speed_ratio},
        {"display_refresh_hz", s.display_refresh_hz},
        {"vertical_sync_suspended", s.vertical_sync_suspended},
        {"oldest_frame_serial", s.oldest_frame_serial},
        {"newest_frame_serial", s.newest_frame_serial},
        {"seek_position", rewind_get_seek_position()}
    };

    if (!s.available)
    {
        result["reason"] = s.unavailable_reason;
        return result;
    }

    result["media_mode"] = s.media_mode;
    result["region"] = s.region;

    // What the recording costs, and where the weight is. In cartridge mode
    // the SF-7000 block is simply absent from this list rather than being
    // recorded as zeroes.
    result["cost"] = {
        {"snapshot_raw_bytes", s.snapshot_raw_bytes},
        {"average_compressed_bytes", s.average_compressed_bytes},
        {"compression_ratio", s.compression_ratio},
        {"memory_bytes", s.memory_bytes},
        {"projected_full_bytes", s.projected_full_bytes},
        {"memory_limit_bytes", s.memory_limit_bytes},
        {"memory_limited", s.memory_limited},
        {"groups_dropped_for_memory", s.groups_dropped_for_memory}
    };

    json sections = json::array();
    for (const RewindSectionWeight& w : s.sections)
        sections.push_back({{"section", w.name}, {"bytes", w.bytes}});
    result["sections"] = sections;

    result["health"] = {
        {"snapshots_dropped", s.snapshots_dropped},
        {"serialization_failures", s.serialization_failures},
        {"average_capture_ms", s.average_capture_ms},
        {"worst_capture_ms", s.worst_capture_ms}
    };

    // The recording is correct whatever paces the machine - it counts VBlanks,
    // not wall-clock time - but if the machine is being driven at a rate that
    // is not its own, everything measured in real seconds is off by the same
    // factor. Said here rather than only in get_sync_settings, because this is
    // where someone is looking when it matters.
    if (s.speed_ratio > 0.0 && std::fabs(s.speed_ratio - 1.0) > 0.01)
    {
        // The cause is the clock alignment (Gaming*), not vertical sync
        // itself - a machine can be scaled to the display with or without
        // waiting for the blank (see PACING_ALIGN_BIT / PACING_VSYNC_BIT in
        // config.h). set_sync_settings takes pacing now, not a video_sync
        // boolean, so the fix has to name a mode that actually exists.
        result["pacing_warning"] =
            config_video_pacing_aligned()
                ? "The machine is running at " +
                      std::to_string(s.speed_ratio) +
                      "x its own speed because its clocks are aligned to the "
                      "host display (pacing '" +
                      std::string(config_video_pacing_name()) +
                      "'). Frames recorded are still correct; anything timed "
                      "in real seconds is not. Set pacing to 'accurate' or "
                      "'accurate_vsync' with set_sync_settings to run it on "
                      "its own crystals."
                : "The machine is running at " +
                      std::to_string(s.speed_ratio) +
                      "x its own speed. Frames recorded are still correct; "
                      "anything timed in real seconds is not.";
    }

    return result;
}

json DebugAdapter::ConfigureRewind(bool enabled, int seconds,
                                   int framesPerSnapshot, int memoryLimitMb)
{
    RewindConfigResult r;
    const std::size_t limit =
        memoryLimitMb > 0 ? static_cast<std::size_t>(memoryLimitMb) * 1024u * 1024u
                          : 0u;

    if (!rewind_configure(enabled, seconds, framesPerSnapshot, limit, &r))
    {
        json fail = {{"success", false},
                     {"error", r.error},
                     {"requested_seconds", r.requested_seconds}};
        if (r.error == "rewind_memory_limit_too_small")
        {
            // Say how much it would take rather than quietly recording less
            // than was asked for.
            fail["required_memory_mb"] =
                (r.required_memory_bytes + (1024u * 1024u) - 1) / (1024u * 1024u);
            fail["memory_limit_mb"] =
                r.memory_limit_bytes / (1024u * 1024u);
        }
        return fail;
    }

    json ok = GetRewindStatus();
    ok["success"] = true;

    // Recording no longer borrows the vertical sync setting, so there is
    // nothing to announce here. It used to: with vertical sync on the machine
    // advanced one frame per refresh, which made every duration in a
    // recording a measure of the monitor. The scheduler paces frames from the
    // machine's own rate now, so the setting no longer affects what is
    // recorded. vertical_sync_suspended stays in the status, always false, so
    // a client reading it does not break.

    return ok;
}

// Where the cursor is, and whether anything is moving. Read-only: after a
// pause, a breakpoint or somebody dragging the timeline in the window, a
// client has no way to know where the recorder ended up unless it can ask.
// Host-side throttling, which is not the same thing as the machine's frame
// rate. The SC-3000's VDP produces 59.9226 frames a second NTSC no matter
// what these are set to; vertical sync decides how fast those frames reach
// the screen in wall-clock time, and therefore how long a recording takes to
// fill. With it on, the emulator is paced by the host display - which on a
// 60 Hz panel is close to the machine's rate by coincidence, and on a 120 Hz
// or a variable-refresh one is not.
json DebugAdapter::GetSyncSettings()
{
    const double refresh = application_get_display_refresh_hz();
    const double native = m_core->GetNativeFrameRate();
    const double effective = m_core->GetEffectiveFrameRate();
    const GearSF7000ClockRates rates = m_core->GetClockDomains().GetRates();

    json result = {
        {"pacing", config_video_pacing_name()},
        {"video_sync", config_video_vsync()},
        {"display_refresh_hz", refresh},
        {"machine_native_fps", native},
        {"machine_effective_fps", effective},
        {"clock_scale", m_core->GetClockScale()},
        {"cpu_clock_hz", rates.cpuHz},
        {"vdp_clock_hz", rates.vdpMasterHz},
        {"fdc_clock_hz", rates.fdcHz}
    };

    // What the scheduler is actually doing, measured rather than inferred.
    // This used to reason about vertical sync dragging the machine off its
    // rate, because it did: the loop ran one emulated frame per iteration and
    // the iteration rate was the refresh rate. That is no longer how frames
    // are paced - see platforms/desktop-shared/scheduler.cpp - so the numbers
    // below are the report, and vertical sync is a presentation setting.
    const SchedulerDiagnostics pacing = scheduler_get_diagnostics();

    result["scheduler"] = {
        {"measured_fps", pacing.measured_fps},
        {"target_fps", pacing.effective_fps},
        {"debt_frames", pacing.debt_frames},
        {"audio_drift_correction", pacing.audio_correction},
        {"catch_up_clamps", pacing.catch_up_clamps},
        {"frames_last_iteration", pacing.frames_last_iteration}
    };

    // Measured against what the scheduler is aiming for, which during fast
    // forward is deliberately not the machine's own rate.
    if (pacing.measured_fps <= 0.0 || pacing.effective_fps <= 0.0)
    {
        result["verdict"] =
            "No frames have been produced yet, so there is nothing to measure.";
    }
    else
    {
        const double error =
            (pacing.measured_fps - pacing.effective_fps) / pacing.effective_fps;
        result["target_error"] = error;

        if (std::fabs(error) <= 0.01)
        {
            result["verdict"] =
                "The machine is running on its own clocks, independently of the "
                "display's refresh rate.";
        }
        else if (error < 0.0)
        {
            result["verdict"] =
                "The machine is running slower than its own rate. The host is "
                "not keeping up, or something outside the scheduler is blocking "
                "the loop.";
        }
        else
        {
            result["verdict"] =
                "The machine is running faster than its own rate, which the "
                "scheduler should not allow - worth reporting.";
        }
    }

    return result;
}

json DebugAdapter::SetSyncSettings(const json& arguments)
{
    if (arguments.contains("pacing"))
    {
        const std::string mode = arguments["pacing"];
        const int resolved = config_video_pacing_from_name(mode.c_str(), -1);
        if (resolved < 0)
            return {{"success", false},
                    {"error", "pacing must be accurate, accurate_vsync, "
                              "gaming or gaming_vsync"}};
        config_video.pacing = resolved;

        // Presentation changes now; the clock alignment is latched at Reset,
        // which is why the reply says so rather than pretending it took.
        renderer_set_vsync(config_video_vsync());
    }

    return GetSyncSettings();
}

json DebugAdapter::GetRewindPosition()
{
    const bool recording = rewind_is_enabled();
    const int count = rewind_get_snapshot_count();
    const int rawPosition = rewind_get_seek_position();

    // -1 means the cursor is not parked anywhere: the machine has run since
    // the last seek, so the live frame is the newest one.
    const int age = rawPosition < 0 ? 0 : rawPosition;

    const bool corePaused = m_core->IsPaused();
    const bool debuggerPaused = emu_is_debugging();

    json result = {
        {"recording", recording},
        {"running", !(corePaused || debuggerPaused)},
        {"paused", corePaused || debuggerPaused},
        {"paused_by_core", corePaused},
        {"paused_by_debugger", debuggerPaused},
        {"scrubbing", rawPosition > 0},
        {"buffer_size", count},
        {"position", count > 0 ? count - age : 0},
        {"age_frames", age}
    };

    if (!recording)
    {
        result["hint"] = "start it with configure_rewind {enabled: true}";
        return result;
    }

    const RewindStatus s = rewind_get_status();
    result["buffer_capacity"] = s.capacity;
    result["buffer_seconds"] = s.buffer_seconds;
    result["buffered_seconds"] = s.buffered_seconds;
    result["max_age_seconds"] = s.max_age_seconds;
    // The machine's own rate, not a round 50/60: at 59.9227 the difference is
    // 0.13%, which is 78 ms across a full minute of recording - and it used to
    // disagree with buffered_seconds in the same reply.
    const double fps = s.frames_per_second > 0.0 ? s.frames_per_second : 60.0;
    result["age_seconds"] = s.frames_per_snapshot * age / fps;
    result["oldest_frame_serial"] = s.oldest_frame_serial;
    result["newest_frame_serial"] = s.newest_frame_serial;

    const CpuStateSnapshot cpu = m_core->GetCpuStateAccess()->GetCpuStateSnapshot();
    char pc[8];
    snprintf(pc, sizeof(pc), "%04X", cpu.pc);
    result["pc"] = pc;

    if (Video* video = m_core->GetVideo())
        result["frame_serial"] =
            video->GetFrameRenderDiagnostics().frameSerial;

    return result;
}

json DebugAdapter::AnalyzeRewindRange(const json& query)
{
    static const int kMaximumSamples = 300;
    static const size_t kMaximumRanges = 8;
    static const size_t kMaximumRangeBytes = 4096;
    static const size_t kMaximumMemoryBytes = 16384;
    static const size_t kMaximumReturnedMemoryBytes = 256 * 1024;

    if (!emu_is_execution_stopped())
        return {{"error", "Pause playback before analyzing a Recorder range"}};

    const int count = rewind_get_snapshot_count();
    if (count <= 0)
        return {{"error", "The Recorder has no snapshots"}};
    if (!query.contains("start_position") || !query.contains("end_position") ||
        !query["start_position"].is_number_integer() ||
        !query["end_position"].is_number_integer())
        return {{"error", "start_position and end_position are required integers"}};

    const int start = query["start_position"].get<int>();
    const int end = query["end_position"].get<int>();
    const int step = query.value("step", 1);
    if (start < 1 || end < start || end > count || step < 1)
        return {{"error", "Range must satisfy 1 <= start_position <= end_position <= snapshot_count and step >= 1"},
                {"snapshot_count", count}};

    std::vector<int> positions;
    for (int position = start; position <= end; position += step)
        positions.push_back(position);
    if (positions.empty() || positions.back() != end)
        positions.push_back(end);
    if (positions.size() > static_cast<size_t>(kMaximumSamples))
        return {{"error", "The requested range exceeds 300 samples; increase step"},
                {"samples_requested", positions.size()}};

    const json ranges = query.value("memory_ranges", json::array());
    if (!ranges.is_array() || ranges.size() > kMaximumRanges)
        return {{"error", "memory_ranges must be an array containing at most 8 ranges"}};

    struct Range { int area; u32 offset; size_t size; std::string name; };
    std::vector<Range> checkedRanges;
    size_t memoryPerSample = 0;
    for (const json& range : ranges)
    {
        if (!range.is_object() || !range.contains("area") ||
            !range.contains("offset") || !range.contains("size") ||
            !range["area"].is_number_integer() ||
            !range["offset"].is_number_integer() ||
            !range["size"].is_number_integer())
            return {{"error", "Each memory range requires integer area, offset and size"}};
        const int area = range["area"].get<int>();
        const int64_t rawOffset = range["offset"].get<int64_t>();
        const int64_t rawSize = range["size"].get<int64_t>();
        const MemoryAreaInfo info = GetMemoryAreaInfo(area);
        if (info.id < 0 || rawOffset < 0 || rawSize < 1 ||
            rawSize > static_cast<int64_t>(kMaximumRangeBytes) ||
            static_cast<u64>(rawOffset) + static_cast<u64>(rawSize) > info.size)
            return {{"error", "Invalid memory range"}, {"area", area},
                    {"offset", rawOffset}, {"size", rawSize}};
        memoryPerSample += static_cast<size_t>(rawSize);
        if (memoryPerSample > kMaximumMemoryBytes)
            return {{"error", "Memory ranges exceed the 16384-byte per-sample limit"}};
        checkedRanges.push_back({area, static_cast<u32>(rawOffset),
                                 static_cast<size_t>(rawSize), info.name});
    }

    const bool includeCpu = query.value("cpu", true);
    const bool includeVdp = query.value("vdp", true);
    const bool changesOnly = query.value("changes_only", true);
    if (!changesOnly && memoryPerSample * positions.size() > kMaximumReturnedMemoryBytes)
        return {{"error", "Full memory samples exceed the 262144-byte result limit; use changes_only or increase step"}};

    std::stringstream saved(std::ios::in | std::ios::out | std::ios::binary);
    size_t savedSize = 0;
    if (!m_core->SaveState(saved, savedSize))
        return {{"error", "Could not preserve the current machine state"}};
    const std::string original = saved.str();
    struct RestoreState
    {
        GearSF7000Core* core;
        const std::string& bytes;
        ~RestoreState()
        {
            core->LoadState(reinterpret_cast<const u8*>(bytes.data()), bytes.size());
        }
    } restore{m_core, original};

    const auto hexBytes = [](const std::vector<u8>& bytes, size_t first, size_t length)
    {
        std::ostringstream out;
        for (size_t i = 0; i < length; ++i)
        {
            if (i) out << ' ';
            out << std::hex << std::uppercase << std::setfill('0') << std::setw(2)
                << static_cast<unsigned int>(bytes[first + i]);
        }
        return out.str();
    };
    const auto hashBytes = [](const std::vector<u8>& bytes)
    {
        u64 hash = 1469598103934665603ULL;
        for (u8 value : bytes) { hash ^= value; hash *= 1099511628211ULL; }
        return hash;
    };

    std::vector<std::vector<u8>> previous(checkedRanges.size());
    json samples = json::array();
    size_t returnedMemoryBytes = 0;
    bool payloadTruncated = false;
    for (size_t sampleIndex = 0; sampleIndex < positions.size(); ++sampleIndex)
    {
        const int position = positions[sampleIndex];
        const int age = count - position;
        std::vector<u8> raw;
        u64 frameSerial = 0;
        if (!rewind_read_snapshot(age, raw, &frameSerial) ||
            !m_core->LoadState(raw.data(), raw.size()))
            return {{"error", "Could not reconstruct Recorder snapshot"},
                    {"position", position}, {"age", age}};

        json sample = {{"position", position}, {"age", age},
                       {"frame_serial", frameSerial}};
        if (includeCpu)
        {
            const CpuStateSnapshot cpu = m_core->GetCpuStateAccess()->GetCpuStateSnapshot();
            sample["cpu"] = {{"pc", cpu.pc}, {"sp", cpu.sp}, {"af", cpu.af},
                             {"bc", cpu.bc}, {"de", cpu.de}, {"hl", cpu.hl},
                             {"ix", cpu.ix}, {"iy", cpu.iy},
                             {"halted", cpu.halted},
                             {"tstates", m_core->GetCpuStateAccess()->GetElapsedTStates()}};
        }
        if (includeVdp)
        {
            Video* video = m_core->GetVideo();
            json registers = json::array();
            for (int i = 0; i < 8; ++i) registers.push_back(video->GetRegisters()[i]);
            const Video::FrameRenderDiagnostics d = video->GetFrameRenderDiagnostics();
            sample["vdp"] = {{"registers", registers},
                             {"status", video->GetStatusReg()},
                             {"legacy_frame_hash", d.legacyFrameHash},
                             {"background_hash", d.streamingBackgroundHash},
                             {"sprite_hash", d.streamingSpriteHash},
                             {"overflow_line", d.streamingOverflowLine},
                             {"overflow_sprite", d.streamingOverflowSprite},
                             {"collision_line", d.streamingCollisionLine}};
        }

        json memory = json::array();
        for (size_t rangeIndex = 0; rangeIndex < checkedRanges.size(); ++rangeIndex)
        {
            const Range& range = checkedRanges[rangeIndex];
            const std::vector<u8> current =
                ReadMemoryArea(range.area, range.offset, range.size);
            json item = {{"area", range.area}, {"name", range.name},
                         {"offset", range.offset}, {"size", current.size()},
                         {"hash", hashBytes(current)}, {"spans", json::array()}};

            size_t i = 0;
            while (i < current.size())
            {
                const bool changed = sampleIndex == 0 || !changesOnly ||
                    previous[rangeIndex].empty() || current[i] != previous[rangeIndex][i];
                if (!changed) { ++i; continue; }
                const size_t first = i++;
                while (i < current.size() &&
                       (sampleIndex == 0 || !changesOnly ||
                        previous[rangeIndex].empty() || current[i] != previous[rangeIndex][i]))
                    ++i;
                const size_t length = i - first;
                if (returnedMemoryBytes + length <= kMaximumReturnedMemoryBytes)
                {
                    item["spans"].push_back({{"offset", range.offset + first},
                                             {"size", length},
                                             {"data", hexBytes(current, first, length)}});
                    returnedMemoryBytes += length;
                }
                else
                    payloadTruncated = true;
            }
            previous[rangeIndex] = current;
            memory.push_back(std::move(item));
        }
        if (!checkedRanges.empty()) sample["memory"] = std::move(memory);
        samples.push_back(std::move(sample));
    }

    return {{"schema_version", 1}, {"rom_crc32", m_core->GetCartridge()->GetCRC()},
            {"snapshot_count", count}, {"start_position", start},
            {"end_position", end}, {"step", step},
            {"sample_count", samples.size()}, {"samples", std::move(samples)},
            {"memory_encoding", "first sample is a baseline; later spans are byte deltas when changes_only is true"},
            {"returned_memory_bytes", returnedMemoryBytes},
            {"payload_truncated", payloadTruncated},
            {"machine_state_restored", true}, {"recorder_cursor_unchanged", true},
            {"executed_frames", 0}};
}

json DebugAdapter::RewindTransport(const std::string& action, int frames)
{
    // The same six operations the transport bar offers, so a client can drive
    // the recorder without reimplementing what the buttons already mean.
    const int count = rewind_get_snapshot_count();

    if (action == "play")
    {
        // Same safe default as F5: inspection must not destroy evidence.
        // emu_debug_continue() restores the newest snapshot before resuming.
        const bool returnedToLive = rewind_get_seek_position() > 0;
        emu_debug_continue();
        return {{"success", true}, {"action", action}, {"paused", false},
                {"mode", "append"}, {"returned_to_live", returnedToLive}};
    }

    if (action == "play_from_here")
    {
        // Explicit destructive counterpart of Ctrl+F5. With no active
        // recording it remains a normal Continue instead of a dead command.
        const bool branched = rewind_is_enabled() &&
                              rewind_get_seek_position() > 0;
        if (branched)
            rewind_commit_seek();
        emu_debug_continue();
        return {{"success", true}, {"action", action}, {"paused", false},
                {"mode", "branch"}, {"branched", branched}};
    }

    if (action == "pause")
    {
        emu_pause();
        return {{"success", true}, {"action", action}, {"paused", true}};
    }

    // Distinguish "the recorder is off" from "the recorder is on but has
    // nothing yet": the first needs a configure_rewind, the second just needs
    // the machine to run for a moment.
    if (!rewind_is_enabled())
        return {{"success", false},
                {"error", "recorder_stopped"},
                {"hint", "start it with configure_rewind {enabled: true}"}};

    if (count <= 0)
        return {{"success", false}, {"error", "no_snapshots_yet"},
                {"hint", "let the machine run; frames are recorded as they complete"}};

    const int position = rewind_get_seek_position() < 0
                             ? 0
                             : rewind_get_seek_position();
    const int step = frames > 0 ? frames : 1;

    int target = position;
    if (action == "step_back")
        target = position + step;
    else if (action == "step_forward")
        target = position - step;
    else if (action == "oldest")
        target = count - 1;
    else if (action == "newest")
        target = 0;
    else
        return {{"success", false}, {"error", "unknown_action"},
                {"actions", json::array({"play", "play_from_here", "pause", "step_back",
                                         "step_forward", "oldest", "newest"})}};

    if (target < 0)
        target = 0;
    if (target > count - 1)
        target = count - 1;

    return RewindSeek(count - target);
}

json DebugAdapter::RewindSeek(int snapshot)
{
    if (!rewind_is_enabled())
        return {{"success", false},
                {"error", "recorder_stopped"},
                {"hint", "start it with configure_rewind {enabled: true}"}};

    const int count = rewind_get_snapshot_count();
    if (count <= 0)
        return {{"success", false}, {"error", "no_snapshots_yet"},
                {"hint", "let the machine run; frames are recorded as they complete"}};

    // Snapshot 1 is the oldest and snapshot_count the newest, matching the
    // numbering a Gearsystem client already speaks. Internally the ring is
    // addressed by age, so the two are converted here in one place.
    if (snapshot < 1 || snapshot > count)
        return {{"success", false},
                {"error", "snapshot_out_of_range"},
                {"snapshot_count", count}};

    // Where the recorder was before this move, so the answer can say how far
    // it travelled and in which direction.
    const int previousAge =
        rewind_get_seek_position() < 0 ? 0 : rewind_get_seek_position();

    const int age = count - snapshot;
    if (!rewind_seek(age))
        return {{"success", false}, {"error", "seek_failed"},
                {"reason", m_core->GetLastStateError()}};

    // A VGM recording active during the seek does not survive it - the
    // recorder is a linear log with no notion of a branch, and resuming from
    // here would re-issue writes it already appended for the future this
    // seek abandons. See rewind_seek()'s own comment. Reported in the
    // response rather than left for a caller to discover from a truncated
    // file the next time they look for one.
    const std::string vgmStopped = rewind_take_vgm_stop_notice();

    const RewindStatus s = rewind_get_status();

    // Three readings of the same landing point, because each answers a
    // different question: how far it just moved, where that is inside the
    // buffer, and which frame of the machine's life it is.
    json result = {
        {"success", true},
        {"paused", true},

        // Movement, signed: positive is forward in time, negative back.
        {"delta_frames", previousAge - age},

        // Position inside the ring: 1 is the oldest frame held.
        {"position", snapshot},
        {"buffer_size", count},
        {"buffer_capacity", s.capacity},
        {"age_frames", age},
        {"age_seconds", s.frames_per_snapshot * age /
                            (s.frames_per_second > 0.0 ? s.frames_per_second
                                                       : 60.0)}
    };

    const CpuStateSnapshot cpu = m_core->GetCpuStateAccess()->GetCpuStateSnapshot();
    char pc[8];
    snprintf(pc, sizeof(pc), "%04X", cpu.pc);
    result["pc"] = pc;

    // The machine's own frame count since it was switched on, which is the
    // number that means the same thing across a whole session.
    if (Video* video = m_core->GetVideo())
        result["frame_serial"] =
            video->GetFrameRenderDiagnostics().frameSerial;

    // Kept so an existing client that reads "snapshot" does not break.
    result["snapshot"] = snapshot;
    result["snapshot_count"] = count;

    if (!vgmStopped.empty())
    {
        result["vgm_recording_stopped"] = true;
        result["vgm_recording_path"] = vgmStopped;
        result["note"] = "This seek stopped an in-progress VGM recording and "
                         "saved everything captured up to this point - "
                         "resuming from an old frame would have re-issued "
                         "writes already written for the future this seek "
                         "abandoned.";
    }

    return result;
}

json DebugAdapter::TriggerNMI(){emu_pause_key_pressed();return {{"success",true},{"signal","nmi"},{"source","SC-3000 reset line / F1"}};} json DebugAdapter::KeyboardText(const std::string& text){std::string error;if(!emu_keyboard_text(text.c_str(),"MCP",&error))return {{"error",error.empty()?"Unable to queue SC-3000 keyboard text":error}};return {{"success",true},{"characters",text.size()},{"timing","initial idle, three matrix polls pressed, two released"}};} json DebugAdapter::CancelKeyboardText(){emu_keyboard_clear_text();return {{"success",true}};}
json DebugAdapter::BasicTyperSetText(const std::string& text, bool send)
{
    gui_debug_basic_typer_set_text(text.c_str());
    if (!send)
        return {{"success",true},{"characters",text.size()},{"sent",false}};
    const bool queued = gui_debug_basic_typer_send("MCP");
    return {{"success",queued},{"characters",text.size()},{"sent",queued},
            {"note",queued?"Typing. Untypable characters, if any, were dropped - the panel says how many.":"Nothing was queued. A sequence may already be running: cancel_keyboard_text first."}};
}

json DebugAdapter::BasicTyperClear(bool cancel_queue)
{
    gui_debug_basic_typer_clear();
    if (cancel_queue)
        emu_keyboard_clear_text();
    return {{"success",true},{"queue_cancelled",cancel_queue}};
}

json DebugAdapter::BasicTyperStatus()
{
    const char* text = gui_debug_basic_typer_get_text();
    return {{"characters",text?strlen(text):0},
            {"text",text?text:""},
            {"typing",emu_keyboard_text_busy()},
            {"typing_source",emu_keyboard_text_source()},
            {"progress",emu_keyboard_text_progress()},
            {"panel_visible",config_debug.show_basic_typer}};
}

json DebugAdapter::LoadBasicProgram(const std::string& file_path, int pointer_block)
{
    const std::filesystem::path path(file_path);
    if (file_path.empty() || !path.is_absolute())
        return {{"error","file_path must be an absolute path"}};

    // -1, the default when the argument is absent, follows the GUI setting.
    // An explicit 0 forces the scan even when the setting pins an address -
    // otherwise there would be no way to ask for Auto from here.
    const int block = pointer_block < 0 ? config_debug.basic_pointer_block : pointer_block;

    std::string status;
    const bool loaded = emu_load_basic_program(file_path.c_str(), block, &status);
    if (!loaded)
        return {{"error",status},{"pointer_block",block}};
    return {{"success",true},{"detail",status},{"pointer_block",block}};
}

json DebugAdapter::SaveBasicProgram(const std::string& file_path, int pointer_block)
{
    const std::filesystem::path path(file_path);
    if (file_path.empty() || !path.is_absolute())
        return {{"error","file_path must be an absolute path"}};

    // -1, the default when the argument is absent, follows the GUI setting.
    // An explicit 0 forces the scan even when the setting pins an address -
    // otherwise there would be no way to ask for Auto from here.
    const int block = pointer_block < 0 ? config_debug.basic_pointer_block : pointer_block;

    std::string status;
    if (!emu_save_basic_program(file_path.c_str(), block, &status))
        return {{"error",status},{"pointer_block",block}};
    return {{"success",true},{"detail",status},{"pointer_block",block}};
}

json DebugAdapter::FindBasicBlocks()
{
    u16 blocks[16];
    const int count = emu_find_basic_blocks(blocks, 16);

    json list = json::array();
    for (int i = 0; i < count; i++)
    {
        char text[8];
        snprintf(text, sizeof(text), "%04X", blocks[i]);
        list.push_back(text);
    }
    return {{"blocks",list},{"configured",config_debug.basic_pointer_block}};
}

json DebugAdapter::KeyboardKey(const std::string& key, const std::string& action)
{
    const std::string wantedAction = LowerAscii(action);
    bool pressed;
    if (wantedAction == "press") pressed = true;
    else if (wantedAction == "release") pressed = false;
    else return {{"error", "action must be \"press\" or \"release\""}, {"action", action}};

    if (!emu_keyboard_key(key.c_str(), pressed))
        return {{"error", "unknown non-printable key label"}, {"key", key}};

    return {{"success", true}, {"key", key}, {"action", wantedAction}};
}
json DebugAdapter::ControllerButton(int player, const std::string& button, const std::string& action)
{
    // The SC-3000 reads either the keyboard matrix or the joystick depending on
    // the machine mode, so keyboard_text cannot start a cartridge game while the
    // machine is in joystick mode. This drives the joystick directly.
    if (player != 1 && player != 2)
        return {{"error", "player must be 1 or 2"}, {"player", player}};

    const std::string wanted = LowerAscii(button);
    GC_Keys key;
    if      (wanted == "up")    key = Key_Up;
    else if (wanted == "down")  key = Key_Down;
    else if (wanted == "left")  key = Key_Left;
    else if (wanted == "right") key = Key_Right;
    else if (wanted == "left_button" || wanted == "fire1" || wanted == "yellow"
             || wanted == "button1" || wanted == "fire")
        key = Key_Left_Button;
    else if (wanted == "right_button" || wanted == "fire2" || wanted == "red"
             || wanted == "button2")
        key = Key_Right_Button;
    // The ColecoVision keypad names this inherited from GearSF7000 - digits,
    // asterisk, hash, blue, purple - were accepted here and forwarded to
    // SK1100::JoystickPressed, whose switch has no case for any of them: they
    // did nothing at all, silently, and reported success while doing it. The
    // SC-3000 joystick has four directions and two buttons; there is no
    // keypad to wire them to. Refused by name so a caller is told rather than
    // left to wonder why a press had no effect.
    else if (wanted == "blue" || wanted == "purple" || wanted == "asterisk" ||
             wanted == "*" || wanted == "hash" || wanted == "#" ||
             (wanted.size() == 1 && wanted[0] >= '0' && wanted[0] <= '9'))
        return {{"error", "The SC-3000 joystick has no keypad: only up, down, left, right and the two fire buttons exist. This name comes from the ColecoVision controller and was silently doing nothing."},
                {"button", button}};
    else
        return {{"error", "unknown button"}, {"button", button}};

    const GC_Controllers controller =
        player == 1 ? Controller_1 : Controller_2;
    const std::string wantedAction = LowerAscii(action);
    if (wantedAction == "press")
        emu_joy_pressed(controller, key);
    else if (wantedAction == "release")
        emu_joy_released(controller, key);
    else if (wantedAction == "press_and_release")
    {
        // Holding needs the machine to advance between the two edges, and this
        // call cannot let it run. Ask for the two edges separately instead.
        return {{"error", "press_and_release needs the machine to run between the two edges: send \"press\", let at least one frame pass, then \"release\""},
                {"action", action}};
    }
    else
        return {{"error", "action must be \"press\" or \"release\""}, {"action", action}};

    return {{"success", true}, {"player", player}, {"button", wanted},
            {"action", wantedAction},
            {"note", "The machine polls once per frame: hold for at least a frame before releasing."}};
}

json DebugAdapter::GetInputState()
{
    // Row 7 of the SK-1100 matrix is where controller_button's writes land -
    // see SK1100::JoystickPressed. Bits 0-5 are player 1 (up, down, left,
    // right, button1, button2), 6-11 are player 2, active low.
    uint16_t rows[8] = {};
    m_core->GetInputRows(rows);
    const uint16_t joystick = rows[7];

    // Not IsSetBit(): it takes a u8, and player 2's bits (6-11) do not all
    // fit in one - bits 8-11 were silently truncated away before any bit test
    // ran, so IsSetBit always read 0 there and every one of those four
    // buttons reported permanently pressed. joystick is 16 bits; the mask has
    // to be evaluated as 16 bits too.
    auto pressed = [joystick](int bit) {
        return ((joystick >> bit) & 1) == 0;
    };
    auto player = [pressed](int offset) {
        return json{
            {"up",      pressed(0 + offset)},
            {"down",    pressed(1 + offset)},
            {"left",    pressed(2 + offset)},
            {"right",   pressed(3 + offset)},
            {"button1", pressed(4 + offset)},
            {"button2", pressed(5 + offset)}
        };
    };

    // What controller_button actually accepts for digits, "blue" and "purple"
    // does not appear here: those keys reach JoystickPressed/Released too,
    // but its switch statement has no case for them (Keypad_*, Key_Blue,
    // Key_Purple), so on this build they are silently no-ops on real hardware
    // paths as well as here. Said explicitly rather than reported as pressed
    // or omitted without comment, so a caller does not waste time chasing a
    // press that cannot have had any effect.
    return {
        {"player1", player(0)},
        {"player2", player(6)}
    };
}

json DebugAdapter::GetKeyboardMode()
{
    return {{"keyboard_mode", emu_get_keyboard_mode()}};
}

json DebugAdapter::SetKeyboardMode(bool enabled)
{
    emu_set_keyboard_mode(enabled);
    return {{"success", true}, {"keyboard_mode", emu_get_keyboard_mode()}};
}
json DebugAdapter::AddDisassemblerBookmark(u16,const std::string&){return unavailable("Disassembler bookmarks");} json DebugAdapter::RemoveDisassemblerBookmark(u16){return unavailable("Disassembler bookmarks");} json DebugAdapter::ListDisassemblerBookmarks(){return unavailable("Disassembler bookmarks");} 
// Symbols, keyed by (bank, address).
//
// On a banked cartridge $8000-$BFFF holds different code depending on the
// mapper's current bank, so a name attached to the address alone would not
// just be missing from the other banks - it would be printed over them,
// confidently and wrongly. That matters more now that the bank is saved and
// rewound: seeking backwards changes the bank underneath you, and an
// address-only symbol would tell a different lie every frame.
//
// These share the disassembler's own table (gui_debug.h) rather than keeping
// a second one, so a symbol added here appears in the disassembler window at
// once and vice versa. Two tables would disagree the moment anyone used the
// side that did not own the one being read.

namespace
{
// Bank 0 is where an unbanked address lives - the .sym parser defaults to it
// when a line carries no BANK: prefix - so it is the sensible answer for a
// caller that omits the bank rather than an error.
json SymbolToJson(int bank, u16 address, const char* text)
{
    char bankHex[8];
    char addrHex[8];
    std::snprintf(bankHex, sizeof(bankHex), "%02X", (unsigned)(bank & 0xFF));
    std::snprintf(addrHex, sizeof(addrHex), "%04X", (unsigned)address);
    return {{"bank", bankHex}, {"address", addrHex}, {"name", text ? text : ""}};
}
}

json DebugAdapter::AddSymbol(u8 bank, u16 address, const std::string& name)
{
    if (name.empty())
        return {{"error", "A symbol needs a name"}};

    gui_debug_set_symbol((int)bank, address, name.c_str());
    return {{"success", true},
            {"symbol", SymbolToJson((int)bank, address, name.c_str())},
            {"total", gui_debug_symbol_count()},
            {"note", "One name per bank:address - this replaced any symbol "
                     "already there."}};
}

json DebugAdapter::RemoveSymbol(u8 bank, u16 address)
{
    const bool removed = gui_debug_remove_symbol((int)bank, address);
    if (!removed)
        return {{"success", false},
                {"error", "No symbol at that bank:address"},
                {"symbol", SymbolToJson((int)bank, address, "")}};

    return {{"success", true},
            {"symbol", SymbolToJson((int)bank, address, "")},
            {"total", gui_debug_symbol_count()}};
}

json DebugAdapter::LoadSymbols(const std::string& file_path)
{
    const int added = gui_debug_load_symbols_file_counted(file_path.c_str());
    if (added < 0)
        return {{"error", "Could not open the symbol file"},
                {"file_path", file_path}};

    // Zero added is not an error but it is worth saying out loud: a file whose
    // every line the parser rejects loads "successfully" and changes nothing.
    json result = {{"success", true},
                   {"file_path", file_path},
                   {"added", added},
                   {"total", gui_debug_symbol_count()}};
    if (added == 0)
        result["note"] = "The file was read but no symbol line was recognised. "
                         "The format is BANK:ADDRESS LABEL (both hex), or "
                         "ADDRESS LABEL for bank 0; ';' starts a comment.";
    return result;
}

json DebugAdapter::ListSymbols()
{
    json list = json::array();
    const int count = gui_debug_symbol_count();
    for (int i = 0; i < count; i++)
    {
        int bank = 0;
        u16 address = 0;
        const char* text = NULL;
        if (gui_debug_get_symbol(i, &bank, &address, &text))
            list.push_back(SymbolToJson(bank, address, text));
    }
    return {{"symbols", list}, {"count", (int)list.size()}};
}

json DebugAdapter::LookupSymbolByName(const std::string& name)
{
    // Every match, not the first: the same name can legitimately exist in
    // more than one bank, and picking one silently would hide that.
    json matches = json::array();
    const int count = gui_debug_symbol_count();
    for (int i = 0; i < count; i++)
    {
        int bank = 0;
        u16 address = 0;
        const char* text = NULL;
        if (gui_debug_get_symbol(i, &bank, &address, &text) &&
            text != NULL && name == text)
            matches.push_back(SymbolToJson(bank, address, text));
    }
    return {{"name", name}, {"matches", matches},
            {"count", (int)matches.size()}};
}

json DebugAdapter::LookupSymbolAtAddress(u8 bank, u16 address)
{
    const int count = gui_debug_symbol_count();
    for (int i = 0; i < count; i++)
    {
        int b = 0;
        u16 a = 0;
        const char* text = NULL;
        if (gui_debug_get_symbol(i, &b, &a, &text) &&
            b == (int)bank && a == address)
            return {{"found", true},
                    {"symbol", SymbolToJson(b, a, text)}};
    }
    return {{"found", false},
            {"symbol", SymbolToJson((int)bank, address, "")}};
}
 json DebugAdapter::ListCallStack(){return unavailable("Call stack");}
json DebugAdapter::SelectMemoryRange(int,int,int){return unavailable("Memory editor selection");} json DebugAdapter::SetMemorySelectionValue(int,u8){return unavailable("Memory editor selection");} json DebugAdapter::GetMemorySelection(int){return unavailable("Memory editor selection");} json DebugAdapter::AddMemoryBookmark(int,int,const std::string&){return unavailable("Memory bookmarks");} json DebugAdapter::RemoveMemoryBookmark(int,int){return unavailable("Memory bookmarks");} json DebugAdapter::ListMemoryBookmarks(int){return unavailable("Memory bookmarks");}

namespace
{
// Cheat-search state, kept separate from the GUI's g_editors[] slots
// (gui_debug_memory.cpp): those are generic display windows repointed at
// whatever the user has open, not one-per-area, so they can't hold a
// stable "previous capture" baseline the way this needs. One MemEditor per
// MCP memory area (see DebugAdapter::GetMemoryAreaInfo) instead, indexed
// the same way.
constexpr int kSearchAreaCount = 5;
MemEditor g_search_editors[kSearchAreaCount];
const u8* g_search_last_ptr[kSearchAreaCount] = {};
u32 g_search_last_size[kSearchAreaCount] = {};
bool g_search_captured[kSearchAreaCount] = {};

// Area 0 ("Z80 address space") has no flat backing buffer - ReadMemoryArea
// synthesizes it byte-by-byte through Memory::Read() to respect bank
// mapping. MemEditor needs a real pointer to sit on, so give it one and
// refresh it in place before every capture/search instead of copying on
// every single byte access.
u8 g_area0_buffer[0x10000];

void RefreshSearchEditor(GearSF7000Core* core, int area, const MemoryAreaInfo& info)
{
    const u8* data = info.data;
    if (area == 0)
    {
        Memory* mem = core->GetMemory();
        for (u32 i = 0; i < info.size; i++)
            g_area0_buffer[i] = mem->Read(static_cast<u16>(i));
        data = g_area0_buffer;
    }

    if (data != g_search_last_ptr[area] || info.size != g_search_last_size[area])
    {
        // The underlying buffer moved or resized (ROM (re)loaded, ejected,
        // etc.) - any earlier capture is against memory that may no longer
        // exist. Rebind and drop it rather than compare against stale data.
        g_search_editors[area].Reset(info.name.c_str(), const_cast<u8*>(data),
            static_cast<int>(info.size), 0, 1);
        g_search_last_ptr[area] = data;
        g_search_last_size[area] = info.size;
        g_search_captured[area] = false;
    }
}

bool ValidSearchArea(int area) { return area >= 0 && area < kSearchAreaCount; }
}

json DebugAdapter::MemorySearchCapture(int area)
{
    if (!ValidSearchArea(area))
        return {{"error", "Invalid area number"}};

    RefreshSearchEditor(m_core, area, GetMemoryAreaInfo(area));
    g_search_editors[area].SearchCapture();
    g_search_captured[area] = true;

    return {{"success", true}, {"area", area}};
}

json DebugAdapter::MemorySearch(int area, const std::string& op, const std::string& compare_type, int compare_value, const std::string& data_type, bool narrow_previous)
{
    if (!ValidSearchArea(area))
        return {{"error", "Invalid area number"}};

    int opIndex;
    if      (op == "<")  opIndex = 0;
    else if (op == ">")  opIndex = 1;
    else if (op == "==") opIndex = 2;
    else if (op == "!=") opIndex = 3;
    else if (op == "<=") opIndex = 4;
    else if (op == ">=") opIndex = 5;
    else return {{"error", "Invalid operator"}};

    int compareTypeIndex;
    if      (compare_type == "previous") compareTypeIndex = 0;
    else if (compare_type == "value")    compareTypeIndex = 1;
    else if (compare_type == "address")  compareTypeIndex = 2;
    else return {{"error", "Invalid compare_type"}};

    int dataTypeIndex;
    if      (data_type == "hex")      dataTypeIndex = 0;
    else if (data_type == "signed")   dataTypeIndex = 1;
    else if (data_type == "unsigned") dataTypeIndex = 2;
    else return {{"error", "Invalid data_type"}};

    MemoryAreaInfo info = GetMemoryAreaInfo(area);
    RefreshSearchEditor(m_core, area, info);
    if (!g_search_captured[area])
        return {{"error", "No capture for this area yet - call memory_search_capture first (or again: the underlying buffer changed since the last capture)"}};

    int compareValue = compare_value;
    if (compareTypeIndex == 2)
    {
        if (compare_value < 0 || static_cast<u32>(compare_value) >= info.size)
            return {{"error", "Compare address outside memory area"}};
    }

    const int count = g_search_editors[area].PerformSearch(opIndex, compareTypeIndex, compareValue, dataTypeIndex, narrow_previous);
    std::vector<MemEditor::Search>* results = g_search_editors[area].GetSearchResults();

    json out;
    out["area"] = area;
    out["count"] = count;
    out["fields"] = json::array({"address", "value", "previous"});
    out["results"] = json::array();

    const int maxResults = count > 1000 ? 1000 : count;
    for (int i = 0; i < maxResults; i++)
    {
        const MemEditor::Search& s = (*results)[i];
        std::ostringstream addr;
        addr << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << s.address;
        out["results"].push_back(json::array({addr.str(), s.value, s.prev_value}));
    }
    if (count > 1000) out["total_matches"] = count;

    return out;
}

json DebugAdapter::MemoryFindBytes(int area, const std::string& hex_bytes)
{
    if (!ValidSearchArea(area))
        return {{"error", "Invalid area number"}};
    if (hex_bytes.empty())
        return {{"error", "Empty hex byte string"}};

    RefreshSearchEditor(m_core, area, GetMemoryAreaInfo(area));

    int addresses[100];
    const int count = g_search_editors[area].FindBytesSequence(hex_bytes.c_str(), addresses, 100);

    json out;
    out["area"] = area;
    out["count"] = count;
    out["addresses"] = json::array();
    const int maxResults = count > 100 ? 100 : count;
    for (int i = 0; i < maxResults; i++)
    {
        std::ostringstream addr;
        addr << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << addresses[i];
        out["addresses"].push_back(addr.str());
    }
    return out;
}

json DebugAdapter::MemoryFindBytesAdvanced(int area, const std::string& pattern, int wildcard_limit)
{
    if (!ValidSearchArea(area))
        return {{"error", "Invalid area number"}};
    if (pattern.empty())
        return {{"error", "Empty pattern"}};

    RefreshSearchEditor(m_core, area, GetMemoryAreaInfo(area));

    int addresses[100];
    int lengths[100];
    const int count = g_search_editors[area].FindBytesSequenceAdvanced(pattern.c_str(), wildcard_limit, addresses, lengths, 100);

    json out;
    out["area"] = area;
    out["count"] = count;
    out["results"] = json::array();
    const int maxResults = count > 100 ? 100 : count;
    for (int i = 0; i < maxResults; i++)
    {
        std::ostringstream addr;
        addr << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << addresses[i];
        out["results"].push_back({{"address", addr.str()}, {"length", lengths[i]}});
    }
    return out;
}

namespace
{
// The trace is not a second event system: it is the rule system with the
// filters taken off. Each selected kind becomes one rule covering the whole
// address range with no value condition and Log as its only action, so it
// records everything of that kind and never pauses.
//
// They are named so they can be told apart from a user's own rules and
// removed as a group.
const char* const kTracePrefix = "trace:";

struct TraceKind
{
    const char* key;
    DebugEventCategory category;
};

const TraceKind kTraceKinds[] = {
    { "cpu",          DebugEventCategory::CpuExecute  },
    { "memory",       DebugEventCategory::CpuMemory   },
    { "vram",         DebugEventCategory::Vram        },
    { "vdp_register", DebugEventCategory::VdpRegister },
    { "io_port",      DebugEventCategory::Io          },
    { "ppi",          DebugEventCategory::Ppi         },
    { "fdc",          DebugEventCategory::Fdc         },
    { "tape",         DebugEventCategory::Tape        },
    { "psg",          DebugEventCategory::Audio       },
    { "ay",           DebugEventCategory::AyExpansion },
    { "video_timing", DebugEventCategory::VideoTiming },
    { "device_state", DebugEventCategory::DeviceState },
};

bool is_trace_rule(const DebugRule& rule)
{
    return rule.name.compare(0, strlen(kTracePrefix), kTracePrefix) == 0;
}

void remove_trace_rules(DebugEventManager* manager)
{
    std::vector<u32> doomed;
    for (const DebugRule& rule : manager->GetRules())
        if (is_trace_rule(rule))
            doomed.push_back(rule.id);
    for (u32 id : doomed)
        manager->RemoveRule(id);
}
}

json DebugAdapter::SetTraceLog(bool enabled, u32 flags)
{
    return SetTraceLogDetailed(enabled, flags, json::object());
}

json DebugAdapter::SetTraceLogDetailed(bool enabled, u32 flags,
                                       const json& arguments)
{
    DebugEventManager* manager = m_core->GetMemory()->GetDebugEventManager();

    remove_trace_rules(manager);

    // clear is "empty the ring first" (see its schema description) - a
    // starting condition, not something stopping implies. It used to clear
    // whenever the trace was being turned off regardless of what the caller
    // asked, which silently threw away exactly what a caller stopping the
    // trace to go read it would want kept.
    if (arguments.value("clear", false))
        manager->ClearEvents();

    if (arguments.contains("capacity"))
        manager->SetEventCapacity(arguments["capacity"].get<size_t>());

    json armed = json::array();
    json refused = json::array();

    if (enabled)
    {
        // Named booleans are what a caller should use; the legacy bitmask
        // still works, and with neither given everything is armed, because
        // asking to trace without saying what almost always means "all of it".
        const bool named = std::any_of(
            std::begin(kTraceKinds), std::end(kTraceKinds),
            [&](const TraceKind& kind) { return arguments.contains(kind.key); });

        for (size_t i = 0; i < sizeof(kTraceKinds) / sizeof(kTraceKinds[0]); ++i)
        {
            const TraceKind& kind = kTraceKinds[i];

            bool wanted;
            if (named)
                wanted = arguments.value(kind.key, false);
            else if (flags != 0 && flags != 0xFF)
                wanted = (flags & (1u << i)) != 0;
            else
                wanted = true;

            if (!wanted)
                continue;

            // Each category publishes a different address range and a
            // different set of access types, so "everything" has to be asked
            // of the category rather than assumed - a rule covering more than
            // a category supports is rejected, which is how this should work.
            const DebugEventCategoryCapabilities capabilities =
                GetDebugEventCategoryCapabilities(kind.category);

            DebugRule rule;
            rule.name = std::string(kTracePrefix) + kind.key;
            rule.description = "trace: everything of this kind, no filter";
            rule.category = kind.category;
            rule.addressStart = 0;
            rule.addressEnd = capabilities.targetMax;
            rule.accessMask = capabilities.supportedAccesses;
            rule.useValueCondition = false;
            rule.valueCondition = DebugValueCondition::Any;
            rule.breakOnHit = 0;                       // never pause
            rule.actions = DebugEventAction_Log;       // record only
            rule.oneShot = false;

            if (const char* reason = ValidateDebugRule(rule))
            {
                // Said out loud rather than silently skipped: an empty "armed"
                // list with no explanation is exactly the kind of quiet
                // failure this project keeps having to remove.
                refused.push_back({{"kind", kind.key}, {"reason", reason}});
                continue;
            }

            if (manager->AddRule(rule) != 0)
                armed.push_back(kind.key);
        }
    }

    return {
        {"success", true},
        {"enabled", enabled && !armed.empty()},
        {"armed", armed},
        {"refused", refused},
        {"capacity", manager->GetEventCapacity()},
        {"events_held", manager->GetEventCount()},
        {"note", "Tracing is the debug rule system with its filters removed, "
                 "so a trace and your own rules share one ring - read it with "
                 "get_trace_log or get_debug_events."}
    };
}

json DebugAdapter::GetTraceLog(int start, int count)
{
    DebugEventManager* manager = m_core->GetMemory()->GetDebugEventManager();

    // GetDebugEvents already builds the entries and wraps them; the window
    // and the ring's own numbers are what this adds.
    json events = GetDebugEvents({{"count", count > 0 ? count : 4096}})["events"];

    // What it cost is reported alongside: dropped counts events the ring
    // overwrote, and a trace that quietly loses the middle of a sequence is
    // worse than one that admits it.
    return {
        {"events", events},
        {"count", events.size()},
        {"capacity", manager->GetEventCapacity()},
        {"events_held", manager->GetEventCount()},
        {"dropped", manager->GetDroppedEventCount()},
        {"start", start}
    };
}

namespace
{
// Multi-value live monitor. One flat list shared across all memory areas
// (watch ids are independent of area, unlike the search-tool state above
// which is one MemEditor per area) plus a single AND/OR trigger condition -
// deliberately one active condition, not a tree, to keep the MCP surface
// small; clearing and re-setting it is cheap.
struct WatchEntry
{
    int id; int area; u32 address; std::string data_type; int size; std::string label;
    bool frozen; std::vector<u8> frozen_bytes;
    bool has_data; std::vector<u8> current_bytes; std::vector<u8> previous_bytes;
};
struct WatchConditionEntry { int watch_id; std::string op; json value; };

std::vector<WatchEntry> g_watches;
int g_watch_next_id = 1;
std::vector<WatchConditionEntry> g_watch_conditions;
std::string g_watch_condition_mode = "AND";
bool g_watch_condition_active = false;
bool g_watch_condition_triggered = false;

bool ValidWatchDataType(const std::string& t) { return t == "unsigned" || t == "signed" || t == "hex" || t == "text"; }

// data_type "hex" defaults a bare digit string ("90") to base 16, matching
// what watch_list prints back for a hex watch - std::stoll's base-0
// autodetect would otherwise read that same "90" as decimal 90 (0x5A), so a
// condition copy-pasted straight from watch_list's own hex output would
// silently never match. "0x.." prefixes still work either way.
long long WatchParseInt(const json& v, const std::string& data_type)
{
    if (v.is_number()) return v.get<long long>();
    if (v.is_string())
    {
        try { return std::stoll(v.get_ref<const std::string&>(), nullptr, data_type == "hex" ? 16 : 0); }
        catch (...) { return 0; }
    }
    return 0;
}

std::vector<u8> WatchEncode(const std::string& data_type, int size, const json& value)
{
    std::vector<u8> out(size, 0);
    if (data_type == "text")
    {
        std::string s = value.is_string() ? value.get<std::string>() : value.dump();
        for (int i = 0; i < size && i < (int)s.size(); i++) out[i] = (u8)s[i];
        return out;
    }
    long long n = WatchParseInt(value, data_type);
    for (int i = 0; i < size; i++) out[i] = (u8)((n >> (8 * i)) & 0xFF);
    return out;
}

long long WatchDecodeNumeric(const std::vector<u8>& bytes, const std::string& data_type)
{
    long long n = 0;
    for (int i = (int)bytes.size() - 1; i >= 0; i--) n = (n << 8) | bytes[i];
    if (data_type == "signed")
    {
        if (bytes.size() == 1 && (n & 0x80)) n -= 0x100;
        else if (bytes.size() == 2 && (n & 0x8000)) n -= 0x10000;
    }
    return n;
}

std::string WatchSanitizeText(const std::vector<u8>& bytes)
{
    std::string s; s.reserve(bytes.size());
    for (u8 b : bytes) s.push_back((b >= 0x20 && b < 0x7F) ? (char)b : '.');
    return s;
}

json WatchDisplay(const WatchEntry& e, const std::vector<u8>& bytes)
{
    if (bytes.empty()) return nullptr;
    if (e.data_type == "text") return WatchSanitizeText(bytes);
    long long n = WatchDecodeNumeric(bytes, e.data_type);
    if (e.data_type == "hex")
    {
        std::ostringstream h;
        h << std::hex << std::uppercase << std::setfill('0') << std::setw(e.size * 2)
          << (n & (e.size == 1 ? 0xFFLL : 0xFFFFLL));
        return h.str();
    }
    return n;
}

WatchEntry* FindWatch(int id)
{
    for (auto& e : g_watches) if (e.id == id) return &e;
    return nullptr;
}

json WatchEntryToJson(const WatchEntry& e)
{
    std::ostringstream addr;
    addr << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << e.address;
    return {
        {"id", e.id}, {"area", e.area}, {"address", addr.str()}, {"data_type", e.data_type},
        {"size", e.size}, {"label", e.label}, {"frozen", e.frozen},
        {"value", WatchDisplay(e, e.current_bytes)}, {"previous_value", WatchDisplay(e, e.previous_bytes)},
        {"changed", e.has_data && e.current_bytes != e.previous_bytes}
    };
}

// "changed"/"increased"/"decreased" compare this tick's value against last
// tick's, not against a caller-supplied constant - e.g. "lives just went
// down" regardless of what the exact before/after numbers are. They need a
// real previous sample, so a watch that hasn't ticked twice yet (has_data
// but current_bytes == previous_bytes on its very first read) can't satisfy
// "changed" by definition - that first tick sets both to the same value on
// purpose (see WatchTick), so this isn't a special case to guard separately.
bool WatchConditionHolds(const WatchConditionEntry& c)
{
    WatchEntry* e = FindWatch(c.watch_id);
    if (!e || e->current_bytes.empty()) return false;

    if (c.op == "changed")
        return e->current_bytes != e->previous_bytes;

    if (e->data_type == "text")
    {
        std::string current = WatchSanitizeText(e->current_bytes);
        std::string target = c.value.is_string() ? c.value.get<std::string>() : c.value.dump();
        bool eq = (current == target);
        return c.op == "==" ? eq : !eq;
    }

    long long current = WatchDecodeNumeric(e->current_bytes, e->data_type);

    if (c.op == "increased" || c.op == "decreased")
    {
        long long previous = WatchDecodeNumeric(e->previous_bytes, e->data_type);
        return c.op == "increased" ? (current > previous) : (current < previous);
    }

    long long target = WatchParseInt(c.value, e->data_type);
    if (c.op == "==") return current == target;
    if (c.op == "!=") return current != target;
    if (c.op == ">")  return current > target;
    if (c.op == "<")  return current < target;
    if (c.op == ">=") return current >= target;
    if (c.op == "<=") return current <= target;
    return false;
}
}

json DebugAdapter::WatchAdd(int area, u32 address, const std::string& data_type, int size, const std::string& label)
{
    if (!ValidSearchArea(area))
        return {{"error", "Invalid area number"}};
    if (!ValidWatchDataType(data_type))
        return {{"error", "Invalid data_type: must be unsigned, signed, hex, or text"}};
    if (data_type == "text") { if (size < 1 || size > 64) return {{"error", "text size must be 1-64 bytes"}}; }
    else if (size != 1 && size != 2) return {{"error", "size must be 1 or 2 bytes for unsigned/signed/hex watches"}};

    MemoryAreaInfo info = GetMemoryAreaInfo(area);
    if (!info.size || address + (u32)size > info.size)
        return {{"error", "Address range outside memory area"}};

    WatchEntry entry{};
    entry.id = g_watch_next_id++;
    entry.area = area; entry.address = address; entry.data_type = data_type; entry.size = size; entry.label = label;
    g_watches.push_back(entry);

    return {{"success", true}, {"id", entry.id}};
}

json DebugAdapter::WatchRemove(int id)
{
    auto it = std::find_if(g_watches.begin(), g_watches.end(), [id](const WatchEntry& e) { return e.id == id; });
    if (it == g_watches.end())
        return {{"error", "No watch with that id"}};
    g_watches.erase(it);

    // A condition referencing this id would silently stop meaning what the
    // caller set it up to mean - drop the whole set rather than leave a
    // dangling reference that never evaluates true.
    bool hadCondition = std::any_of(g_watch_conditions.begin(), g_watch_conditions.end(),
        [id](const WatchConditionEntry& c) { return c.watch_id == id; });
    if (hadCondition) { g_watch_conditions.clear(); g_watch_condition_active = false; g_watch_condition_triggered = false; }

    return {{"success", true}};
}

json DebugAdapter::WatchList()
{
    json out = json::array();
    for (const auto& e : g_watches) out.push_back(WatchEntryToJson(e));
    return {{"watches", out}};
}

json DebugAdapter::WatchFreeze(int id, const json& value)
{
    WatchEntry* e = FindWatch(id);
    if (!e) return {{"error", "No watch with that id"}};
    e->frozen_bytes = WatchEncode(e->data_type, e->size, value);
    e->frozen = true;
    WriteMemoryArea(e->area, e->address, e->frozen_bytes);
    return {{"success", true}, {"id", id}};
}

json DebugAdapter::WatchUnfreeze(int id)
{
    WatchEntry* e = FindWatch(id);
    if (!e) return {{"error", "No watch with that id"}};
    e->frozen = false;
    return {{"success", true}, {"id", id}};
}

json DebugAdapter::WatchSetCondition(const json& conditions, const std::string& mode)
{
    if (mode != "AND" && mode != "OR")
        return {{"error", "mode must be AND or OR"}};
    if (!conditions.is_array() || conditions.empty())
        return {{"error", "conditions must be a non-empty array"}};

    // "changed"/"increased"/"decreased" compare this tick's value against
    // last tick's, not against the caller's "value" - so "value" is only
    // required for the constant-comparison operators.
    static const std::vector<std::string> numericOps = {"==", "!=", ">", "<", ">=", "<=", "changed", "increased", "decreased"};
    static const std::vector<std::string> textOps = {"==", "!=", "changed"};
    static const std::vector<std::string> transitionOps = {"changed", "increased", "decreased"};

    std::vector<WatchConditionEntry> parsed;
    for (const auto& c : conditions)
    {
        if (!c.contains("id") || !c.contains("op"))
            return {{"error", "each condition needs id and op"}};
        int wid = c["id"].get<int>();
        WatchEntry* e = FindWatch(wid);
        if (!e) return {{"error", "Unknown watch id in condition"}, {"id", wid}};
        std::string op = c["op"].get<std::string>();
        const auto& allowed = (e->data_type == "text") ? textOps : numericOps;
        if (std::find(allowed.begin(), allowed.end(), op) == allowed.end())
            return {{"error", "Invalid operator for this watch's data_type"}, {"id", wid}, {"op", op}};
        bool isTransitionOp = std::find(transitionOps.begin(), transitionOps.end(), op) != transitionOps.end();
        if (!isTransitionOp && !c.contains("value"))
            return {{"error", "this operator needs a value"}, {"id", wid}, {"op", op}};
        parsed.push_back({wid, op, isTransitionOp ? json(nullptr) : c["value"]});
    }

    g_watch_conditions = parsed;
    g_watch_condition_mode = mode;
    g_watch_condition_active = true;
    g_watch_condition_triggered = false;
    return {{"success", true}, {"count", (int)parsed.size()}, {"mode", mode}};
}

json DebugAdapter::WatchClearCondition()
{
    g_watch_conditions.clear();
    g_watch_condition_active = false;
    g_watch_condition_triggered = false;
    return {{"success", true}};
}

json DebugAdapter::WatchConditionStatus()
{
    json conds = json::array();
    for (const auto& c : g_watch_conditions)
    {
        WatchEntry* e = FindWatch(c.watch_id);
        conds.push_back({{"id", c.watch_id}, {"label", e ? e->label : ""}, {"op", c.op}, {"value", c.value}});
    }
    return {{"active", g_watch_condition_active}, {"triggered", g_watch_condition_triggered},
             {"mode", g_watch_condition_mode}, {"conditions", conds}};
}

bool DebugAdapter::WatchTick()
{
    for (auto& e : g_watches)
    {
        if (e.frozen)
            WriteMemoryArea(e.area, e.address, e.frozen_bytes);

        std::vector<u8> data = ReadMemoryArea(e.area, e.address, e.size);
        if (data.empty()) continue;
        if (!e.has_data) { e.current_bytes = data; e.previous_bytes = data; e.has_data = true; }
        else { e.previous_bytes = e.current_bytes; e.current_bytes = data; }
    }

    if (!g_watch_condition_active || g_watch_condition_triggered || g_watch_conditions.empty())
        return false;

    bool result = (g_watch_condition_mode == "AND");
    for (const auto& c : g_watch_conditions)
    {
        bool holds = WatchConditionHolds(c);
        result = (g_watch_condition_mode == "AND") ? (result && holds) : (result || holds);
    }

    if (!result) return false;
    g_watch_condition_triggered = true;
    return true;
}
#endif
