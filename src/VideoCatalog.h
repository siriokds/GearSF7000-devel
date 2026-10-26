// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
/* Immutable video selection and output catalogue. JSON is decoded once. */
#pragma once
#include <map>
#include <string>
#include <vector>
#include "ISystemBus.h"

struct VideoOutputValue
{
    std::string code;
    int configValue = -1;
    int deviceValue = -1;
    std::vector<std::string> contexts;
};
struct VideoOutputCapability
{
    std::string code;
    std::vector<std::string> contexts;
    std::vector<VideoOutputValue> values;
};
struct VideoOutputOverride
{
    std::string capability, value;
    std::map<std::string, std::vector<std::string>> values;
};
struct VideoOutputProfile
{
    std::string code;
    std::map<std::string, std::vector<std::string>> values;
    std::map<std::string, std::map<std::string, std::string>> defaults;
    std::vector<VideoOutputOverride> overrides;
    std::string defaultCrop;
    std::map<std::string, std::string> cropSources;
};
struct VideoMachineOption
{
    std::string code, device, variant, core, outputProfile, label;
    SystemVideoDevice legacyDevice;
    int legacyConfig = 0;
    bool available = false;
};

struct VideoOutputPolicy
{
    std::map<std::string, std::vector<int>> values;
    std::map<std::string, int> defaults;
    bool fullRaster = false;
    bool Allows(const std::string& capability, int value) const;
    int Resolve(const std::string& capability, int value) const;
};
struct VideoShaderParameter
{
    std::string key;
    float minimum = 0, maximum = 0, defaultValue = 0;
    std::vector<std::pair<std::string, int>> choices;
};

class VideoCatalog
{
public:
    // Throws on malformed data; no partially populated catalog is published.
    static VideoCatalog Parse(const std::string& devices, const std::string& capabilities,
                              const std::string& profiles, const std::string& machines);
    static VideoCatalog LoadDirectory(const std::string& directory);
    const VideoMachineOption* FindOption(const std::string& code) const;
    const VideoMachineOption* FindLegacyOption(SystemVideoDevice device) const;
    std::vector<const VideoMachineOption*> MachineOptions(SystemModel model) const;
    const VideoMachineOption* DefaultOption(SystemModel model) const;
    const VideoMachineOption* DeclaredDefaultOption(SystemModel model) const;
    bool IsDeclared(SystemModel model, const std::string& option) const;
    const VideoOutputProfile* Profile(const std::string& code) const;
    const VideoOutputCapability* Capability(const std::string& code) const;
    const VideoOutputPolicy& OutputPolicy(const std::string& option, bool debug, bool fullRaster) const;
    const VideoShaderParameter* ShaderParameter(const std::string& key) const;
private:
    struct Machine { std::string defaultOption; std::vector<std::string> options; };
    std::map<std::string, VideoMachineOption> options_;
    std::map<int, Machine> machines_;
    std::map<std::string, VideoOutputProfile> profiles_;
    std::map<std::string, VideoOutputCapability> capabilities_;
    std::map<std::string, VideoOutputPolicy> outputPolicies_;
    std::map<std::string, VideoShaderParameter> shaderParameters_;
};

const VideoCatalog& GetVideoCatalog();
const std::vector<const VideoMachineOption*>& GetMachineVideoOptions(SystemModel model);
const VideoMachineOption* GetDefaultMachineVideoOption(SystemModel model);

// Keeps the current implementation. A unique same-core chip alternative may
// satisfy the frequency; absent/ambiguous alternatives return nullptr.
const VideoMachineOption* FindMachineVideoOptionForFrequency(SystemModel model,
    const std::string& current, GC_VideoFrequencyFamily frequency);
