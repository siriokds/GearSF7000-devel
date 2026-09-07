/*
 * GearSC3000 - SC-3000 libretro core
 * Copyright (C) 2021 Ignacio Sanchez
 * Copyright (C) 2026 Saverio Russo
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

#include "libretro.h"
#include "libretro_core_options.h"

#include "../../src/GearSF7000Core.h"
#include "../../src/Memory.h"
#include "../../src/Video.h"

namespace
{
constexpr unsigned kPadCount = 2;
constexpr size_t kSystemRamSize = MAX_SRAM_SIZE;

retro_environment_t environ_cb = nullptr;
retro_video_refresh_t video_cb = nullptr;
retro_audio_sample_t audio_cb = nullptr;
retro_audio_sample_batch_t audio_batch_cb = nullptr;
retro_input_poll_t input_poll_cb = nullptr;
retro_input_state_t input_state_cb = nullptr;
retro_log_printf_t log_cb = nullptr;

GearSF7000Core* core = nullptr;
u8* frame_buffer = nullptr;
size_t frame_buffer_capacity = 0;
s16 audio_buffer[GC_AUDIO_BUFFER_SIZE] = {};
Cartridge::ForceConfiguration config;

bool supports_input_bitmasks = false;
bool allow_opposing_directions = false;
bool controller_connected[kPadCount] = { true, true };
int vertical_latch[kPadCount] = {};
int horizontal_latch[kPadCount] = {};
float aspect_ratio = 0.0f;
u64 geometry_revision = 0;

void fallback_log(enum retro_log_level, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
}

void ensure_frame_buffer()
{
    if (!core)
        return;
    const GC_VideoFrameDescriptor descriptor = core->GetVideoFrameDescriptor();
    const size_t required = static_cast<size_t>(descriptor.buffer_width) *
        static_cast<size_t>(descriptor.buffer_height) * sizeof(u16);
    if (required <= frame_buffer_capacity)
        return;

    u8* replacement = new (std::nothrow) u8[required]();
    if (!replacement)
        return;
    delete[] frame_buffer;
    frame_buffer = replacement;
    frame_buffer_capacity = required;
}

double frame_rate(const GC_VideoFrameDescriptor& descriptor)
{
    if (descriptor.refresh_rate.numerator > 0 && descriptor.refresh_rate.denominator > 0)
        return static_cast<double>(descriptor.refresh_rate.numerator) /
            static_cast<double>(descriptor.refresh_rate.denominator);
    return descriptor.region == Region_PAL ? 50.0 : 60.0;
}

void fill_geometry(struct retro_game_geometry& geometry)
{
    const GC_VideoFrameDescriptor descriptor = core->GetVideoFrameDescriptor();
    geometry.base_width = static_cast<unsigned>(descriptor.frame_width);
    geometry.base_height = static_cast<unsigned>(descriptor.frame_height);
    geometry.max_width = static_cast<unsigned>(descriptor.buffer_width);
    geometry.max_height = static_cast<unsigned>(descriptor.buffer_height);
    geometry.aspect_ratio = aspect_ratio;
}

void publish_geometry(bool force)
{
    if (!core || !environ_cb)
        return;
    const GC_VideoFrameDescriptor descriptor = core->GetVideoFrameDescriptor();
    if (!force && descriptor.revision == geometry_revision)
        return;
    geometry_revision = descriptor.revision;
    struct retro_game_geometry geometry = {};
    fill_geometry(geometry);
    environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);
}

const char* get_variable(const char* key)
{
    struct retro_variable variable = { key, nullptr };
    return environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &variable)
        ? variable.value : nullptr;
}

void check_variables()
{
    const char* value = get_variable("gearsc3000_up_down_allowed");
    allow_opposing_directions = value && std::strcmp(value, "Enabled") == 0;

    value = get_variable("gearsc3000_timing");
    if (!value || std::strcmp(value, "Auto") == 0)
        config.region = Cartridge::CartridgeUnknownRegion;
    else if (std::strcmp(value, "PAL") == 0 ||
             std::strcmp(value, "PAL (50 Hz)") == 0)
        config.region = Cartridge::CartridgePAL;
    else
        config.region = Cartridge::CartridgeNTSC;

    value = get_variable("gearsc3000_aspect_ratio");
    if (value && std::strcmp(value, "4:3 DAR") == 0)
        aspect_ratio = 4.0f / 3.0f;
    else if (value && std::strcmp(value, "16:9 DAR") == 0)
        aspect_ratio = 16.0f / 9.0f;
    else
        aspect_ratio = 0.0f;

    value = get_variable("gearsc3000_overscan");
    if (core)
    {
        Video::Overscan overscan = Video::OverscanDisabled;
        if (value && (std::strcmp(value, "Borders") == 0 ||
                      std::strcmp(value, "Full (284 width)") == 0))
            overscan = Video::OverscanFull284;
        core->GetVideo()->SetOverscan(overscan);
        ensure_frame_buffer();
    }
}

struct MatrixBinding { unsigned key; int row; int mask; };

const MatrixBinding keyboard_bindings[] = {
    { RETROK_1, 0, 1 }, { RETROK_q, 0, 2 }, { RETROK_a, 0, 4 }, { RETROK_z, 0, 8 },
    { RETROK_COMMA, 0, 32 }, { RETROK_k, 0, 64 }, { RETROK_i, 0, 128 }, { RETROK_8, 0, 256 },
    { RETROK_2, 1, 1 }, { RETROK_w, 1, 2 }, { RETROK_s, 1, 4 }, { RETROK_x, 1, 8 },
    { RETROK_SPACE, 1, 16 }, { RETROK_PERIOD, 1, 32 }, { RETROK_l, 1, 64 },
    { RETROK_o, 1, 128 }, { RETROK_9, 1, 256 },
    { RETROK_3, 2, 1 }, { RETROK_e, 2, 2 }, { RETROK_d, 2, 4 }, { RETROK_c, 2, 8 },
    { RETROK_HOME, 2, 16 }, { RETROK_SLASH, 2, 32 }, { RETROK_SEMICOLON, 2, 64 },
    { RETROK_p, 2, 128 }, { RETROK_0, 2, 256 },
    { RETROK_4, 3, 1 }, { RETROK_r, 3, 2 }, { RETROK_f, 3, 4 }, { RETROK_v, 3, 8 },
    { RETROK_BACKSPACE, 3, 16 }, { RETROK_QUOTE, 3, 64 }, { RETROK_MINUS, 3, 256 },
    { RETROK_5, 4, 1 }, { RETROK_t, 4, 2 }, { RETROK_g, 4, 4 }, { RETROK_b, 4, 8 },
    { RETROK_DOWN, 4, 32 }, { RETROK_RIGHTBRACKET, 4, 64 },
    { RETROK_LEFTBRACKET, 4, 128 }, { RETROK_EQUALS, 4, 256 },
    { RETROK_6, 5, 1 }, { RETROK_y, 5, 2 }, { RETROK_h, 5, 4 }, { RETROK_n, 5, 8 },
    { RETROK_LEFT, 5, 32 }, { RETROK_RETURN, 5, 64 }, { RETROK_BACKSLASH, 5, 256 },
    { RETROK_TAB, 5, 2048 },
    { RETROK_7, 6, 1 }, { RETROK_u, 6, 2 }, { RETROK_j, 6, 4 }, { RETROK_m, 6, 8 },
    { RETROK_RIGHT, 6, 32 }, { RETROK_UP, 6, 64 }, { RETROK_PAUSE, 6, 256 },
    { RETROK_LALT, 6, 512 }, { RETROK_RALT, 6, 512 },
    { RETROK_LCTRL, 6, 1024 }, { RETROK_RCTRL, 6, 1024 },
    { RETROK_LSHIFT, 6, 2048 }, { RETROK_RSHIFT, 6, 2048 }
};

void keyboard_event(bool down, unsigned keycode, uint32_t, uint16_t)
{
    if (!core)
        return;
    for (const MatrixBinding& binding : keyboard_bindings)
        if (binding.key == keycode)
        {
            core->KeyboardMatrixKey(binding.row, binding.mask, down);
            return;
        }
}

void set_key(GC_Controllers controller, GC_Keys key, bool pressed)
{
    if (pressed)
        core->JoystickPressed(controller, key);
    else
        core->JoystickReleased(controller, key);
}

void release_controller(unsigned player)
{
    const GC_Controllers controller = static_cast<GC_Controllers>(player);
    set_key(controller, Key_Up, false);
    set_key(controller, Key_Down, false);
    set_key(controller, Key_Left, false);
    set_key(controller, Key_Right, false);
    set_key(controller, Key_Left_Button, false);
    set_key(controller, Key_Right_Button, false);
    vertical_latch[player] = 0;
    horizontal_latch[player] = 0;
}

void update_input()
{
    if (!core || !input_poll_cb || !input_state_cb)
        return;
    input_poll_cb();

    for (unsigned player = 0; player < kPadCount; ++player)
    {
        if (!controller_connected[player])
        {
            release_controller(player);
            continue;
        }

        int16_t bits = 0;
        if (supports_input_bitmasks)
            bits = input_state_cb(player, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
        else
            for (unsigned id = 0; id <= RETRO_DEVICE_ID_JOYPAD_R3; ++id)
                if (input_state_cb(player, RETRO_DEVICE_JOYPAD, 0, id))
                    bits |= static_cast<int16_t>(1u << id);

        bool up = (bits & (1 << RETRO_DEVICE_ID_JOYPAD_UP)) != 0;
        bool down = (bits & (1 << RETRO_DEVICE_ID_JOYPAD_DOWN)) != 0;
        bool left = (bits & (1 << RETRO_DEVICE_ID_JOYPAD_LEFT)) != 0;
        bool right = (bits & (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT)) != 0;

        if (!allow_opposing_directions)
        {
            if (up && down)
                (vertical_latch[player] < 0 ? up : down) = false;
            else
                vertical_latch[player] = up ? 1 : (down ? -1 : 0);
            if (left && right)
                (horizontal_latch[player] < 0 ? left : right) = false;
            else
                horizontal_latch[player] = left ? 1 : (right ? -1 : 0);
        }

        const GC_Controllers controller = static_cast<GC_Controllers>(player);
        set_key(controller, Key_Up, up);
        set_key(controller, Key_Down, down);
        set_key(controller, Key_Left, left);
        set_key(controller, Key_Right, right);
        set_key(controller, Key_Left_Button,
            (bits & (1 << RETRO_DEVICE_ID_JOYPAD_B)) != 0);
        set_key(controller, Key_Right_Button,
            (bits & (1 << RETRO_DEVICE_ID_JOYPAD_A)) != 0);
    }
}

void set_input_descriptors()
{
    static const struct retro_input_descriptor descriptors[] = {
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Joystick Up" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Joystick Down" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Joystick Left" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Joystick Right" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Trigger 1" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Trigger 2" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Joystick Up" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Joystick Down" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Joystick Left" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Joystick Right" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Trigger 1" },
        { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Trigger 2" },
        { 0, 0, 0, 0, nullptr }
    };
    environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void*)descriptors);
}

bool is_coleco_extension(const char* path)
{
    if (!path)
        return false;
    const char* extension = std::strrchr(path, '.');
    if (!extension)
        return false;
    char lower[5] = {};
    for (int i = 0; i < 4 && extension[i]; ++i)
    {
        const char c = extension[i];
        lower[i] = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return std::strcmp(lower, ".cv") == 0 || std::strcmp(lower, ".col") == 0;
}
}

void retro_set_environment(retro_environment_t cb)
{
    environ_cb = cb;
    gearsc3000_set_core_options(environ_cb);

    static const struct retro_controller_description controllers[] = {
        { "SC-3000 Joystick", RETRO_DEVICE_JOYPAD },
        { "None", RETRO_DEVICE_NONE }
    };
    static const struct retro_controller_info ports[] = {
        { controllers, 2 }, { controllers, 2 }, { nullptr, 0 }
    };
    environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);
    set_input_descriptors();

    static const struct retro_keyboard_callback keyboard = { keyboard_event };
    environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, (void*)&keyboard);
}

void retro_init(void)
{
    static struct retro_log_callback logging;
    log_cb = environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)
        ? logging.log : fallback_log;

    core = new GearSF7000Core();
    core->Init(GC_PIXEL_RGB565);
    core->SetKeyboardMode(true);
    ensure_frame_buffer();
    supports_input_bitmasks = environ_cb &&
        environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, nullptr);
    check_variables();
    log_cb(RETRO_LOG_INFO, "GearSC3000 libretro\n");
}

void retro_deinit(void)
{
    delete[] frame_buffer;
    frame_buffer = nullptr;
    frame_buffer_capacity = 0;
    delete core;
    core = nullptr;
}

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info* info)
{
    std::memset(info, 0, sizeof(*info));
    info->library_name = "GearSC3000";
    info->library_version = "";
    info->need_fullpath = false;
    info->block_extract = false;
    info->valid_extensions = "sg|sc|rom|asc|bin|zip";
}

void retro_get_system_av_info(struct retro_system_av_info* info)
{
    std::memset(info, 0, sizeof(*info));
    const GC_VideoFrameDescriptor descriptor = core->GetVideoFrameDescriptor();
    fill_geometry(info->geometry);
    info->timing.fps = frame_rate(descriptor);
    info->timing.sample_rate = GC_AUDIO_SAMPLE_RATE;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
    if (port >= kPadCount)
        return;
    controller_connected[port] = device != RETRO_DEVICE_NONE;
    if (!controller_connected[port] && core)
        release_controller(port);
}

void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }

void retro_run(void)
{
    bool updated = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
    {
        check_variables();
        publish_geometry(true);
    }

    update_input();
    ensure_frame_buffer();
    int sample_count = 0;
    core->RunToVBlank(frame_buffer, audio_buffer, &sample_count);
    const GC_VideoFrameDescriptor descriptor = core->GetVideoFrameDescriptor();
    publish_geometry(false);

    if (video_cb)
        video_cb(frame_buffer, descriptor.frame_width, descriptor.frame_height,
            descriptor.stride_pixels * sizeof(u16));
    if (audio_batch_cb && sample_count > 0)
        audio_batch_cb(audio_buffer, static_cast<size_t>(sample_count / 2));
}

void retro_reset(void)
{
    check_variables();
    core->ResetROMPreservingRAM(&config);
}

bool retro_load_game(const struct retro_game_info* info)
{
    if (!info || !info->data || info->size == 0 || info->size > 0x7fffffff)
        return false;
    if (is_coleco_extension(info->path))
    {
        log_cb(RETRO_LOG_ERROR, "ColecoVision content is not part of GearSC3000.\n");
        return false;
    }

    enum retro_pixel_format format = RETRO_PIXEL_FORMAT_RGB565;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format))
    {
        log_cb(RETRO_LOG_ERROR, "RGB565 is not supported by this frontend.\n");
        return false;
    }

    check_variables();
    if (!core->LoadROMFromBuffer(static_cast<const u8*>(info->data),
        static_cast<int>(info->size), &config))
    {
        log_cb(RETRO_LOG_ERROR, "Invalid or unsupported SC-3000 cartridge.\n");
        return false;
    }

    core->SetKeyboardMode(true);
    ensure_frame_buffer();
    geometry_revision = 0;
    publish_geometry(true);
    return true;
}

void retro_unload_game(void)
{
    if (core)
        core->EjectCartridge();
}

unsigned retro_get_region(void)
{
    return core && core->GetCartridge()->IsPAL() ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
}

bool retro_load_game_special(unsigned, const struct retro_game_info*, size_t) { return false; }

size_t retro_serialize_size(void)
{
    size_t size = 0;
    if (core)
        core->SaveState(nullptr, size);
    return size;
}

bool retro_serialize(void* data, size_t size)
{
    return core && data && core->SaveState(static_cast<u8*>(data), size);
}

bool retro_unserialize(const void* data, size_t size)
{
    return core && data && core->LoadState(static_cast<const u8*>(data), size);
}

void* retro_get_memory_data(unsigned id)
{
    return id == RETRO_MEMORY_SYSTEM_RAM && core ? core->GetMemory()->GetRam() : nullptr;
}

size_t retro_get_memory_size(unsigned id)
{
    return id == RETRO_MEMORY_SYSTEM_RAM ? kSystemRamSize : 0;
}

void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned, bool, const char*) {}
