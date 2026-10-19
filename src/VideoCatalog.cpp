#include "VideoCatalog.h"
#include "VideoCatalogData.h"
#include "../third_party/nlohmann/json.hpp"
#include <array>
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace {
using Json = nlohmann::json;
void Require(bool ok, const std::string& message)
{
    if (!ok) throw std::runtime_error("Video catalogue: " + message);
}
Json Read(const std::string& text)
{
    std::vector<std::vector<std::string>> keys;
    return Json::parse(text, [&keys](int, Json::parse_event_t event, Json& value) {
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        else if (event == Json::parse_event_t::key) {
            auto& object = keys.back();
            const std::string key = value.get<std::string>();
            Require(std::find(object.begin(), object.end(), key) == object.end(), "duplicate key " + key);
            object.push_back(key);
        } else if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    });
}
std::string File(const std::string& path)
{
    std::ifstream file(path);
    Require(file.good(), "cannot read " + path);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
struct Binding { const char* name; const char* core; SystemVideoDevice device; };
constexpr Binding bindings[] = {
    {"TMS9918", "TMS99xx", SystemVideoDevice::TMS9918},
    {"TMS9918B", "TMS99xx", SystemVideoDevice::TMS9918B},
    {"TMS9928", "TMS99xx", SystemVideoDevice::TMS9928},
    {"TMS9929", "TMS99xx", SystemVideoDevice::TMS9929},
    {"Pico9918", "Pico9918", SystemVideoDevice::Pico9918},
    {"PicoSMSVdp", "PicoSMSvdp", SystemVideoDevice::PicoSMSVdp},
    {"SegaSMSVdpNTSC", "PicoSMSvdp", SystemVideoDevice::SegaSMSVdpNTSC},
    {"SegaGameGearVdp", "PicoSMSvdp", SystemVideoDevice::SegaGameGearVdp}
};
bool Contains(const std::vector<std::string>& list, const std::string& value)
{ return std::find(list.begin(), list.end(), value) != list.end(); }
}

VideoCatalog VideoCatalog::Parse(const std::string& devicesText, const std::string& capabilitiesText,
                                 const std::string& profilesText, const std::string& machinesText)
{
    const auto devices = Read(devicesText), caps = Read(capabilitiesText), profiles = Read(profilesText), machines = Read(machinesText);
    Require(devices.at("schema_version") == 2 && caps.at("schema_version") == 1 &&
            profiles.at("schema_version") == 2 && machines.at("schema_version") == 1, "unsupported schema");
    VideoCatalog result;
    std::map<std::string, Json> deviceByCode;
    for (const auto& device : devices.at("devices")) {
        const auto code = device.at("code").get<std::string>();
        Require(deviceByCode.emplace(code, device).second, "duplicate device " + code);
    }
    for (const auto& c : caps.at("capabilities")) {
        VideoOutputCapability capability;
        capability.code = c.at("code");
        capability.contexts = c.value("contexts", std::vector<std::string>{});
        for (const auto& v : c.at("values")) {
            VideoOutputValue value{v.at("code"), v.value("config_value", -1), v.value("device_value", -1), v.value("contexts", std::vector<std::string>{})};
            Require(std::none_of(capability.values.begin(), capability.values.end(), [&](const auto& old) { return old.code == value.code; }), "duplicate value");
            capability.values.push_back(std::move(value));
        }
        Require(result.capabilities_.emplace(capability.code, capability).second, "duplicate capability");
    }
    for (const auto& shader : caps.at("shaders")) {
        for (const auto& p : shader.at("parameters")) {
            VideoShaderParameter parameter;
            parameter.key = p.at("key"); parameter.defaultValue = p.at("default");
            if (p.at("type") == "number") {
                parameter.minimum = p.at("min"); parameter.maximum = p.at("max");
                Require(parameter.minimum <= parameter.defaultValue && parameter.defaultValue <= parameter.maximum,
                    "invalid shader range/default");
            } else {
                Require(p.at("type") == "enum", "unknown shader parameter type");
                bool validDefault = false;
                for (const auto& v : p.at("values")) {
                    parameter.choices.emplace_back(v.at("code").get<std::string>(), v.at("value").get<int>());
                    validDefault |= parameter.defaultValue == v.at("value").get<int>();
                }
                Require(validDefault, "invalid shader enum default");
            }
            Require(result.shaderParameters_.emplace(parameter.key, parameter).second, "duplicate shader parameter");
        }
    }
    for (const auto& [code, p] : profiles.at("profiles").items()) {
        VideoOutputProfile profile;
        profile.code = code;
        profile.values = p.at("capabilities").get<decltype(profile.values)>();
        profile.defaults = p.at("defaults").get<decltype(profile.defaults)>();
        for (const auto& [cap, allowed] : profile.values) {
            const auto* definition = result.Capability(cap);
            Require(definition && !allowed.empty(), "unknown capability " + cap);
            for (const auto& value : allowed)
                Require(std::any_of(definition->values.begin(), definition->values.end(), [&](const auto& v) { return v.code == value; }), "unknown value " + value);
        }
        for (const auto& [context, defaults] : profile.defaults)
            for (const auto& [cap, value] : defaults)
                Require(profile.values.count(cap) && Contains(profile.values.at(cap), value), "invalid profile default");
        for (const auto& o : p.value("overrides", Json::array())) {
            VideoOutputOverride rule;
            rule.capability = o.at("when").at("capability"); rule.value = o.at("when").at("value");
            Require(profile.values.count(rule.capability) && Contains(profile.values.at(rule.capability), rule.value), "invalid override condition");
            rule.values = o.at("capabilities").get<decltype(rule.values)>();
            for (const auto& [cap, values] : rule.values)
                for (const auto& v : values) Require(profile.values.count(cap) && Contains(profile.values.at(cap), v), "override expands capability");
            profile.overrides.push_back(std::move(rule));
        }
        profile.defaultCrop = p.at("default_crop");
        for (const auto& [region, definition] : p.at("crop_regions").items()) {
            const auto source = definition.at("source").get<std::string>();
            Require(source == "visible_area" || source == "content_area", "unknown crop source");
            profile.cropSources.emplace(region, source);
        }
        Require(profile.cropSources.count(profile.defaultCrop), "invalid default crop");
        result.profiles_.emplace(code, std::move(profile));
    }
    for (const auto& o : machines.at("options")) {
        VideoMachineOption option;
        option.code = o.at("code"); option.device = o.at("device"); option.variant = o.at("variant");
        option.core = o.at("core"); option.outputProfile = o.at("output_profile"); option.legacyConfig = o.at("legacy_config_value");
        Require(deviceByCode.count(option.device), "unknown device " + option.device);
        const auto& d = deviceByCode.at(option.device);
        Require(d.at("core_variants").contains(option.core), "unknown core " + option.core);
        const auto supported = d.at("core_variants").at(option.core).get<std::vector<std::string>>();
        Require(Contains(supported, option.variant), "unsupported variant " + option.variant);
        bool variantFound = false;
        for (const auto& v : d.at("variants")) if (v.at("code") == option.variant) {
            Require(v.at("status") == "implemented", "unimplemented option");
            option.label = d.at("name").get<std::string>() + " / " + v.at("name").get<std::string>() + " (" + option.core + ")";
            variantFound = true;
        }
        Require(variantFound && result.Profile(option.outputProfile), "unknown variant/profile");
        bool bound = false;
        for (const auto& b : bindings) if (o.at("legacy_device") == b.name) {
            Require(option.core == b.core && option.legacyConfig == GetSystemVideoDeviceConfigValue(b.device), "incorrect factory binding");
            option.legacyDevice = b.device;
            option.available = IsSystemVideoDeviceImplemented(b.device);
            bound = true;
        }
        Require(bound, "unknown factory binding");
        Require(result.options_.emplace(option.code, option).second, "duplicate option " + option.code);
    }
    for (const auto& m : machines.at("machines")) {
        const int id = m.at("model_id");
        Require(id >= 0 && id <= static_cast<int>(SystemModel::GameGearExport), "invalid model id");
        Machine machine{m.at("default_option"), m.at("options").get<std::vector<std::string>>()};
        Require(Contains(machine.options, machine.defaultOption), "default outside machine");
        std::vector<std::string> seen;
        for (const auto& code : machine.options) {
            Require(result.FindOption(code) && !Contains(seen, code), "unknown/duplicate machine option");
            seen.push_back(code);
        }
        Require(result.machines_.emplace(id, machine).second, "duplicate machine id");
    }
    Require(result.machines_.size() == 17, "incomplete machines");
    // Resolve the small set of output policies once, outside frame rendering.
    for (const auto& [code, option] : result.options_)
        for (bool debug : {false, true}) for (bool full : {false, true}) {
            const auto& profile = *result.Profile(option.outputProfile);
            const std::string context = debug ? "debug" : "computer";
            auto allowed = profile.values;
            VideoOutputPolicy policy;
            policy.fullRaster = debug && allowed.count("FULL_RASTER") && Contains(allowed.at("FULL_RASTER"), "ENABLED");
            if (full && policy.fullRaster)
                for (const auto& rule : profile.overrides)
                    if (rule.capability == "FULL_RASTER" && rule.value == "ENABLED")
                        for (const auto& [cap, values] : rule.values) allowed[cap] = values;
            for (const auto& [cap, codes] : allowed) {
                const auto& definition = *result.Capability(cap);
                if (!definition.contexts.empty() && !Contains(definition.contexts, context)) continue;
                for (const auto& v : definition.values) {
                    if (!Contains(codes, v.code) || (!v.contexts.empty() && !Contains(v.contexts, context))) continue;
                    const int frontendValue = cap == "OVERSCAN" ? v.deviceValue : v.configValue;
                    if (frontendValue >= 0) {
                        policy.values[cap].push_back(frontendValue);
                        const auto defaults = profile.defaults.find(context);
                        if (defaults != profile.defaults.end()) {
                            const auto value = defaults->second.find(cap);
                            if (value != defaults->second.end() && value->second == v.code) policy.defaults[cap] = frontendValue;
                        }
                    }
                }
            }
            // Frontend 4 means full raster, never device API FULL_320 (also 4).
            if (policy.fullRaster) policy.values["OVERSCAN"].push_back(4);
            result.outputPolicies_.emplace(code + (debug ? ":debug" : ":computer") + (full ? ":full" : ":visible"), std::move(policy));
        }
    return result;
}
VideoCatalog VideoCatalog::LoadDirectory(const std::string& directory)
{
    return Parse(File(directory + "/video_devices.json"), File(directory + "/video_output_capabilities.json"),
                 File(directory + "/video_output_profiles.json"), File(directory + "/video_machine_options.json"));
}
const VideoMachineOption* VideoCatalog::FindOption(const std::string& code) const
{ const auto i = options_.find(code); return i == options_.end() ? nullptr : &i->second; }
const VideoMachineOption* VideoCatalog::FindLegacyOption(SystemVideoDevice device) const
{ for (const auto& [code, option] : options_) if (option.legacyDevice == device) return &option; return nullptr; }
const VideoOutputProfile* VideoCatalog::Profile(const std::string& code) const
{ const auto i = profiles_.find(code); return i == profiles_.end() ? nullptr : &i->second; }
const VideoOutputCapability* VideoCatalog::Capability(const std::string& code) const
{ const auto i = capabilities_.find(code); return i == capabilities_.end() ? nullptr : &i->second; }
const VideoMachineOption* VideoCatalog::DeclaredDefaultOption(SystemModel model) const
{
    const auto i = machines_.find(static_cast<int>(model));
    return i == machines_.end() ? nullptr : FindOption(i->second.defaultOption);
}
const VideoMachineOption* VideoCatalog::DefaultOption(SystemModel model) const
{
    if (!IsValidSystemModel(model)) return nullptr;
    const auto* option = DeclaredDefaultOption(model);
    return option && option->available ? option : nullptr;
}
std::vector<const VideoMachineOption*> VideoCatalog::MachineOptions(SystemModel model) const
{
    std::vector<const VideoMachineOption*> result;
    if (!DefaultOption(model)) return result;
    for (const auto& code : machines_.at(static_cast<int>(model)).options) {
        const auto* option = FindOption(code);
        if (option->available) result.push_back(option);
    }
    return result;
}
const VideoCatalog& GetVideoCatalog()
{
    static const auto catalog = VideoCatalog::Parse(VideoCatalogData::devices, VideoCatalogData::capabilities,
                                                   VideoCatalogData::profiles, VideoCatalogData::machines);
    return catalog;
}
const std::vector<const VideoMachineOption*>& GetMachineVideoOptions(SystemModel model)
{
    static const auto lists = [] {
        std::array<std::vector<const VideoMachineOption*>, 17> result;
        for (size_t i = 0; i < result.size(); ++i) result[i] = GetVideoCatalog().MachineOptions(static_cast<SystemModel>(i));
        return result;
    }();
    static const std::vector<const VideoMachineOption*> empty;
    const size_t index = static_cast<size_t>(model);
    return index < lists.size() ? lists[index] : empty;
}
const VideoMachineOption* GetDefaultMachineVideoOption(SystemModel model)
{ return GetVideoCatalog().DefaultOption(model); }

bool VideoOutputPolicy::Allows(const std::string& capability, int value) const
{
    const auto i = values.find(capability);
    return i != values.end() && std::find(i->second.begin(), i->second.end(), value) != i->second.end();
}
int VideoOutputPolicy::Resolve(const std::string& capability, int value) const
{
    if (Allows(capability, value)) return value;
    const auto i = defaults.find(capability);
    return i == defaults.end() ? 0 : i->second;
}
const VideoOutputPolicy& VideoCatalog::OutputPolicy(const std::string& option, bool debug, bool fullRaster) const
{
    const auto i = outputPolicies_.find(option + (debug ? ":debug" : ":computer") + (fullRaster ? ":full" : ":visible"));
    static const VideoOutputPolicy empty;
    return i == outputPolicies_.end() ? empty : i->second;
}
const VideoShaderParameter* VideoCatalog::ShaderParameter(const std::string& key) const
{
    const auto i = shaderParameters_.find(key);
    return i == shaderParameters_.end() ? nullptr : &i->second;
}

bool VideoCatalog::IsDeclared(SystemModel model, const std::string& option) const
{
    const auto i = machines_.find(static_cast<int>(model));
    return i != machines_.end() && Contains(i->second.options, option);
}
const VideoMachineOption* FindMachineVideoOptionForFrequency(SystemModel model,
    const std::string& current, GC_VideoFrequencyFamily frequency)
{
    const auto* origin = current.empty() ? GetDefaultMachineVideoOption(model) : GetVideoCatalog().FindOption(current);
    if (!origin || !origin->available || !GetVideoCatalog().IsDeclared(model, origin->code)) return nullptr;
    const auto matches = [&](const VideoMachineOption* option) {
        const auto* descriptor = FindVideoDeviceDescriptor(option->legacyDevice);
        return descriptor && descriptor->frequency_family == frequency;
    };
    if (matches(origin)) return origin;
    const VideoMachineOption* sameDevice = nullptr;
    const VideoMachineOption* sameCore = nullptr;
    int deviceCount = 0, coreCount = 0;
    for (const auto* option : GetMachineVideoOptions(model))
        if (option->core == origin->core && matches(option)) {
            sameCore = option; ++coreCount;
            if (option->device == origin->device) { sameDevice = option; ++deviceCount; }
        }
    if (deviceCount) return deviceCount == 1 ? sameDevice : nullptr;
    return coreCount == 1 ? sameCore : nullptr;
}
