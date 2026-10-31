/*
 * GearSF7000 - the Palettes panel
 * Copyright (C) 2026  Saverio Russo
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "gui_debug_palettes.h"

#include <cstdio>
#include <cstring>

#include "config.h"
#include "emu.h"
#include "gui.h"
#include "imgui/imgui.h"
#include "imgui/colors.h"
#include "../../src/gearsf7000.h"
#include "../../src/IVideoDevice.h"
#include "../../src/IVideoDebugProvider.h"

// The live palette of whatever video device the machine has, sixteen colours
// a row (after Gearsystem's panel), with what the device stores under each
// swatch: the red, green and blue values in their own colours for a 12-bit
// palette (after Geargrafx's), the colour and luminance of a TMS9918B entry,
// the index of a fixed TMS9918A colour, the byte of an SMS CRAM entry. The
// backdrop R7 selects is outlined.

namespace
{
// Pale, so a row of digits reads as text rather than as three bright colours.
const ImVec4 pale_red(1.0f, 0.72f, 0.72f, 1.0f);
const ImVec4 pale_green(0.72f, 1.0f, 0.72f, 1.0f);
const ImVec4 pale_blue(0.75f, 0.82f, 1.0f, 1.0f);

// The CRAM byte (--BBGGRR) an entry was written with: each two-bit component
// was widened to a nibble as 0, 5, A, F. -1 when the entry never came through
// CRAM (a TMS default still in PRAM), which has no byte.
int sms_cram_byte(u16 v)
{
    const int r = (v >> 8) & 0x0F, g = (v >> 4) & 0x0F, b = v & 0x0F;
    if (r % 5 || g % 5 || b % 5)
        return -1;
    return (b / 5) << 4 | (g / 5) << 2 | (r / 5);
}

// R7 of the first register bank: the backdrop colour on every TMS-like VDP.
// In the SMS's Mode 4 it indexes the sprite half of CRAM; in picoSMSvdp's TMS
// modes the fixed colours after it.
int backdrop_index(IVideoDevice* device, const GC_VideoPaletteView& view)
{
    IVideoDebugProvider* provider = device->GetDebugProvider();
    if (!provider || provider->GetRegisterBankCount() == 0)
        return -1;
    GC_VideoRegisterBankDescriptor bank;
    if (!provider->DescribeRegisterBank(0, bank) || !bank.code || bank.register_count < 8)
        return -1;
    u32 r0 = 0, r7 = 0;
    if (!provider->ReadRegisterSnapshot(bank.code, 7, r7))
        return -1;
    provider->ReadRegisterSnapshot(bank.code, 0, r0);
    const char* key = device->GetDeviceDescriptor().key;
    const bool sms = key && std::strstr(key, "SMS") != nullptr;
    int index = static_cast<int>(r7 & 0x0F);
    if (view.fixed_from)
        index += static_cast<int>(view.fixed_from);
    else if (sms && (r0 & 0x04))
        index += 16;
    return index < static_cast<int>(view.color_count) ? index : -1;
}

void draw_native(const GC_VideoPaletteView& view, std::size_t i)
{
    if (view.fixed_from && i >= view.fixed_from)
    {
        ImGui::TextColored(gray, "$%X", static_cast<unsigned>(i - view.fixed_from));
        return;
    }
    switch (view.native_kind)
    {
        case GC_VideoPaletteView::NativeRgb444:
        {
            const u16 v = static_cast<const u16*>(view.native)[i];
            ImGui::TextColored(pale_red, "%X", (v >> 8) & 0x0F);
            ImGui::SameLine(0, 0);
            ImGui::TextColored(pale_green, "%X", (v >> 4) & 0x0F);
            ImGui::SameLine(0, 0);
            ImGui::TextColored(pale_blue, "%X", v & 0x0F);
            break;
        }
        case GC_VideoPaletteView::NativeSmsByte:
        {
            const u16 v = static_cast<const u16*>(view.native)[i];
            const int byte = sms_cram_byte(v);
            if (byte >= 0)
                ImGui::TextColored(white, "$%02X", byte);
            else
                ImGui::TextColored(gray, "%03X", v & 0x0FFF);
            break;
        }
        case GC_VideoPaletteView::NativeTms9918B:
        {
            const u8 v = static_cast<const u8*>(view.native)[i];
            ImGui::TextColored(white, "%X", v & 0x0F);
            ImGui::SameLine(0, 0);
            ImGui::TextColored(gray, "L%d", (v >> 4) & 0x03);
            break;
        }
        default:
            ImGui::TextColored(gray, "$%X", static_cast<unsigned>(i & 0x0F));
            break;
    }
}
}

void gui_debug_palettes_window(void)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    gui_set_next_window_size_em(31.1f, 11.1f);
    if (!ImGui::Begin("Palettes###palettes", &config_debug.show_palettes))
    {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }
    ImGui::PushFont(gui_default_font, gui_get_default_font_size());

    GearSF7000Core* core = emu_get_core();
    const VideoDeviceHandle handle = core->AcquireVideoDevice();
    GC_VideoPaletteView view;
    if (!core->IsVideoDeviceHandleCurrent(handle) || !handle.device->GetPaletteView(view) || !view.IsValid())
    {
        ImGui::TextDisabled("No palette from this video device.");
        ImGui::PopFont();
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }

    const char* name = handle.device->GetDeviceDescriptor().display_name;
    ImGui::TextColored(cyan, "%s", name ? name : "Video");
    ImGui::SameLine();
    ImGui::TextColored(gray, "- %zu colours%s", view.color_count,
        view.native_kind == GC_VideoPaletteView::NativeRgb444 ? ", R G B"
        : view.native_kind == GC_VideoPaletteView::NativeSmsByte ? ", CRAM byte --BBGGRR"
        : view.native_kind == GC_VideoPaletteView::NativeTms9918B ? ", colour + luminance" : ", fixed");
    const int backdrop = backdrop_index(handle.device, view);
    const char* key = handle.device->GetDeviceDescriptor().key;
    const bool sms = key && std::strstr(key, "SMS") != nullptr && view.color_count >= 32;

    const float swatch = ImGui::GetTextLineHeight() * 1.6f;
    const float cell = swatch + 8.0f;
    const std::size_t rows = (view.color_count + 15) / 16;
    for (std::size_t row = 0; row < rows; ++row)
    {
        if (view.fixed_from && row * 16 == view.fixed_from)
            ImGui::TextColored(green, "TMS (fixed, drawn in the TMS modes)");
        else if (sms && row < 2)
            ImGui::TextColored(view.fixed_from ? gray : green, row == 0
                ? (view.fixed_from ? "CRAM 0 (not drawn in a TMS mode)" : "CRAM 0 (BG)")
                : (view.fixed_from ? "CRAM 1 (not drawn in a TMS mode)" : "CRAM 1 (BG + SPRITES)"));
        ImGui::TextColored(gray, "%02zX", row * 16);
        ImGui::SameLine();
        const float start = ImGui::GetCursorPosX();
        for (std::size_t col = 0; col < 16 && row * 16 + col < view.color_count; ++col)
        {
            const std::size_t i = row * 16 + col;
            const u8* c = view.rgb888 + i * 3;
            ImGui::SetCursorPosX(start + col * cell);
            char id[24];
            std::snprintf(id, sizeof(id), "##pal%zu", i);
            ImGui::ColorButton(id, ImVec4(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, 1.0f),
                ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(swatch, swatch));
            if (static_cast<int>(i) == backdrop)
                ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin() - ImVec2(2, 2),
                    ImGui::GetItemRectMax() + ImVec2(2, 2), IM_COL32(255, 255, 255, 255), 0.0f, 0, 2.0f);
            if (ImGui::IsItemHovered())
            {
                char native[32] = "";
                if (view.fixed_from && i >= view.fixed_from)
                    std::snprintf(native, sizeof(native), "  TMS colour $%X, fixed",
                        static_cast<unsigned>(i - view.fixed_from));
                else if (view.native_kind == GC_VideoPaletteView::NativeRgb444)
                    std::snprintf(native, sizeof(native), "  $%03X", static_cast<const u16*>(view.native)[i] & 0x0FFF);
                else if (view.native_kind == GC_VideoPaletteView::NativeSmsByte)
                {
                    const int byte = sms_cram_byte(static_cast<const u16*>(view.native)[i]);
                    if (byte >= 0)
                        std::snprintf(native, sizeof(native), "  $%02X", byte);
                    else
                        std::snprintf(native, sizeof(native), "  not written");
                }
                else if (view.native_kind == GC_VideoPaletteView::NativeTms9918B)
                    std::snprintf(native, sizeof(native), "  $%02X", static_cast<const u8*>(view.native)[i]);
                ImGui::SetTooltip("Colour $%02zX  #%02X%02X%02X%s%s", i, c[0], c[1], c[2], native,
                    static_cast<int>(i) == backdrop ? "\nBackdrop (R7)" : "");
            }
            if (col < 15)
                ImGui::SameLine();
        }
        // What the device stores, under each swatch.
        ImGui::TextUnformatted(" ");
        for (std::size_t col = 0; col < 16 && row * 16 + col < view.color_count; ++col)
        {
            ImGui::SameLine();
            ImGui::SetCursorPosX(start + col * cell);
            draw_native(view, row * 16 + col);
        }
    }

    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleVar();
}
