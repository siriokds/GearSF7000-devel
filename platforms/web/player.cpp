// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#include "../libretro/libretro.h"
#include "../../src/Cartridge.h"

#include <emscripten.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" void gearsc3000_set_target_frame_rate(double fps);
extern "C" double gearsc3000_get_effective_frame_rate(void);
extern "C" int gearsc3000_is_internal_mapper(void);
extern "C" int gearsc3000_is_asc16l_mapper(void);

namespace {
retro_keyboard_callback keyboard = {};
std::vector<std::uint16_t> pixels;
std::vector<std::int16_t> samples;
unsigned width = 0;
unsigned height = 0;
unsigned pads[2] = {};
double fps = 60.0;
bool loaded = false;
unsigned mapper = 0;
unsigned asc16l_ram_kb = 16;
unsigned timing = 1;
bool borders = false;
bool variables_changed = false;
struct MapperOption {
    const char* name;
    Cartridge::CartridgeTypes type;
};
const MapperOption mapper_options[] = {
    {"Auto", Cartridge::CartridgeNotSupported},
    {"SC-3000 2 KB", Cartridge::SC3000_2K},
    {"SC-3000 32 KB", Cartridge::SC3000_32K},
    {"SC-3000 16 KB + 2 KB", Cartridge::SC3000_16K_AT_8000},
    {"ASC16L", Cartridge::SC3000_ASC16L},
    {"Survivors Multicart", Cartridge::SC3000_SURVIVORS_MULTICART},
    {"Survivors Megacart", Cartridge::SC3000_SURVIVORS_MEGACART},
    {"SG-1000 1 KB", Cartridge::SG1000_1K},
    {"SG-1000 16 KB", Cartridge::SG1000_16K},
    {"SG-1000 Othello 2 KB", Cartridge::SG1000_OTHELLO_2K},
    {"SG-1000 The Castle 8 KB", Cartridge::SG1000_CASTLE_8K},
    {"SG-1000 Dahjee A", Cartridge::SG1000_DAHJEE_TYPE_A},
    {"SG-1000 Dahjee B", Cartridge::SG1000_DAHJEE_TYPE_B},
    {"SG-1000 Sega 315-5208", Cartridge::SG1000_SEGA_315_5208},
    {"SG-1000 Sega 315-5208 + Dahjee B", Cartridge::SG1000_SEGA_315_5208_DAHJEE_B},
    {"Master System Mapper", Cartridge::SMS_SEGA_MAPPER}
};

// The core's messages go to the browser console at their own level: without
// this the core writes them all to stderr, which the page shows as errors.
void log(enum retro_log_level level, const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof text, format, args);
    va_end(args);
    std::size_t length = std::strlen(text);
    while (length && text[length - 1] == '\n') text[--length] = 0;
    int flags = EM_LOG_CONSOLE;
    if (level == RETRO_LOG_ERROR) flags |= EM_LOG_ERROR;
    else if (level == RETRO_LOG_WARN) flags |= EM_LOG_WARN;
    else if (level == RETRO_LOG_INFO) flags |= EM_LOG_INFO;
    else flags |= EM_LOG_DEBUG;
    emscripten_log(flags, "%s", text);
}

bool environment(unsigned command, void* data) {
    switch (command) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        if (data) static_cast<retro_log_callback*>(data)->log = log;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return data && *static_cast<retro_pixel_format*>(data) == RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
        if (data) keyboard = *static_cast<retro_keyboard_callback*>(data);
        return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        if (data) fps = static_cast<retro_system_av_info*>(data)->timing.fps;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        if (data) *static_cast<bool*>(data) = variables_changed;
        variables_changed = false;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE:
        if (data) {
            auto* variable = static_cast<retro_variable*>(data);
            if (variable->key && std::strcmp(variable->key, "gearsc3000_mapper") == 0) {
                variable->value = mapper_options[mapper].name;
                return true;
            }
            if (variable->key && std::strcmp(variable->key, "gearsc3000_asc16l_ram") == 0) {
                variable->value = asc16l_ram_kb == 8 ? "8 KB" : "16 KB";
                return true;
            }
            if (variable->key && std::strcmp(variable->key, "gearsc3000_timing") == 0) {
                variable->value = timing == 2 ? "PAL" : timing == 1 ? "NTSC" : "Auto";
                return true;
            }
            if (variable->key && std::strcmp(variable->key, "gearsc3000_overscan") == 0) {
                variable->value = borders ? "Borders" : "Disabled";
                return true;
            }
        }
        return false;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        return true;
    default:
        return false;
    }
}

void video(const void* data, unsigned w, unsigned h, size_t pitch) {
    if (!data || !w || !h) return;
    width = w;
    height = h;
    pixels.resize(static_cast<size_t>(w) * h);
    for (unsigned y = 0; y < h; ++y) {
        const auto* row = reinterpret_cast<const std::uint16_t*>(
            static_cast<const std::uint8_t*>(data) + static_cast<size_t>(y) * pitch);
        std::memcpy(pixels.data() + static_cast<size_t>(y) * w, row,
            static_cast<size_t>(w) * sizeof(std::uint16_t));
    }
}

size_t audio(const std::int16_t* data, size_t frames) {
    if (data && frames) samples.assign(data, data + frames * 2);
    return frames;
}

void poll_input() {}

std::int16_t input(unsigned port, unsigned device, unsigned, unsigned id) {
    if (port >= 2 || device != RETRO_DEVICE_JOYPAD) return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return static_cast<std::int16_t>(pads[port]);
    return id < 16 && (pads[port] & (1u << id)) ? 1 : 0;
}
}

extern "C" {
int web_init() {
    retro_set_environment(environment);
    retro_set_video_refresh(video);
    retro_set_audio_sample_batch(audio);
    retro_set_input_poll(poll_input);
    retro_set_input_state(input);
    retro_init();
    return 1;
}

int web_load(const void* data, unsigned size) {
    if (loaded) retro_unload_game();
    gearsc3000_set_target_frame_rate(0.0);
    loaded = false;
    pixels.clear();
    samples.clear();
    pads[0] = pads[1] = 0;
    if (!data || !size) return 0;
    if (size >= 8 && std::memcmp(data, "SCLS0002", 8) == 0) {
        mapper = 0;
        variables_changed = true;
    }
    if (mapper && !Cartridge::IsTypeCompatible(mapper_options[mapper].type,
        static_cast<int>(size), nullptr)) return 0;
    const retro_game_info game = { "game.rom", data, size, nullptr };
    loaded = retro_load_game(&game);
    if (loaded) {
        variables_changed = true;
        retro_system_av_info av = {};
        retro_get_system_av_info(&av);
        fps = av.timing.fps;
    }
    return loaded ? 1 : 0;
}

void web_run() {
    if (!loaded) return;
    samples.clear();
    retro_run();
}

void web_reset() { if (loaded) retro_reset(); }
void web_set_mapper(unsigned value) {
    if (value < sizeof(mapper_options) / sizeof(mapper_options[0])) mapper = value;
}
void web_set_asc16l_ram(unsigned kib) { asc16l_ram_kb = kib == 8 ? 8 : 16; }
void web_set_timing(unsigned value) { if (value <= 2) timing = value; }
void web_set_borders(int enabled) {
    const bool next = enabled != 0;
    if (borders != next) {
        borders = next;
        variables_changed = true;
    }
}
double web_set_target_fps(double target) {
    if (!loaded) return 0.0;
    gearsc3000_set_target_frame_rate(target);
    retro_reset();
    return gearsc3000_get_effective_frame_rate();
}
void web_pad(unsigned port, unsigned mask) { if (port < 2) pads[port] = mask; }
void web_key(int down, unsigned keycode) {
    if (loaded && keyboard.callback) keyboard.callback(down != 0, keycode, 0, 0);
}
unsigned web_width() { return width; }
unsigned web_height() { return height; }
const std::uint16_t* web_pixels() { return pixels.data(); }
const std::int16_t* web_audio() { return samples.data(); }
unsigned web_audio_frames() { return static_cast<unsigned>(samples.size() / 2); }
double web_fps() { return fps > 1 ? fps : 60.0; }
unsigned web_ram_bytes() { return static_cast<unsigned>(retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM)); }
int web_is_internal_mapper() { return loaded && gearsc3000_is_internal_mapper(); }
int web_is_asc16l_mapper() { return loaded && gearsc3000_is_asc16l_mapper(); }
}
