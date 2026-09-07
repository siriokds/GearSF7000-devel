/*
 * GearSF7000 - SC-3000/SF-7000 Emulator
 * Copyright (C) 2026 Saverio Russo
 */

#include "gui_debug_audio.h"

#include <cstdio>

#include "imgui/imgui.h"
#include "imgui/colors.h"
#include "implot/implot.h"
#include "config.h"
#include "emu.h"
#include "gui.h"
#include "../../src/gearsf7000.h"
#include "../../src/MachineIOPorts.h"

namespace
{

// Rows are taller than their text so the waveform has room, which leaves
// every other cell floating at the top of its row unless it is nudged down.
void center_in_row(float row_height, float item_height)
{
    const float offset = (row_height - item_height) * 0.5f;
    if (offset > 0.0f)
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offset);
}

// Horizontal half of the same idea. Kept separate because the vertical
// offset uses the row height passed in, while this one can measure the
// cell for itself.
void center_in_cell(float item_width)
{
    const float offset = (ImGui::GetContentRegionAvail().x - item_width) * 0.5f;
    if (offset > 0.0f)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
}

// Both axes at once, for the common case of a value sitting alone in a cell.
void center_text_in_cell(float row_height, const char* text)
{
    center_in_row(row_height, ImGui::GetTextLineHeight());
    center_in_cell(ImGui::CalcTextSize(text).x);
}

// Ported from Gearsystem's gui_debug_psg.cpp. The zero-crossing search finds
// a rising edge and centers the plot window on it, which is what makes the
// waveform look stationary like a real oscilloscope's trigger instead of a
// picture that scrolls past at the tone's own frequency.
// width/height are passed in so the caller decides how much of its table
// cell to hand over; -1 for width means "fill the column", the normal case.
void draw_psg_channel_waveform(Audio* audio, int channel, float width, float height)
{
    const blip_sample_t* raw = audio->GetPsgDebugSamples(channel);
    long count = audio->GetPsgDebugSampleCount(channel);

    ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(1, 1));
    const ImPlotAxisFlags flags = ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_NoTickLabels |
        ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoHighlight | ImPlotAxisFlags_Lock |
        ImPlotAxisFlags_NoTickMarks;

    ImGui::PushID(channel);
    // Always the same height, empty plot included: a cell with nothing
    // drawn otherwise takes only as much vertical room as its row's text,
    // and four rows of mismatched heights is what happens right after
    // opening the panel, before the chip has written anything yet.
    if (ImPlot::BeginPlot("##wave", ImVec2(width, height), ImPlotFlags_CanvasOnly))
    {
        ImPlot::SetupAxes("x", "y", flags, flags);

        if (raw && count >= 2)
        {
            static float wave_buffer[4][4096];
            float* wave = wave_buffer[channel];
            if (count > 4096)
                count = 4096;

            // Each channel is normalized to its own peak so its trace fills
            // the box. That deliberately discards relative loudness between
            // channels - the Att column already states the level as a
            // number, and what the plot is for is the shape: duty, edges,
            // the noise pattern. A channel left at its true scale is
            // unreadable, because one oscillator alone never gets near full
            // scale: Sms_Apu::volume divides by osc_count * 128 * 2 so four
            // channels plus drive and tape can sum without clipping, which
            // puts even a maxed-out square at about a tenth of full range.
            float peak = 0.0f;
            for (long i = 0; i < count; i++)
            {
                const float v = (float)raw[i] / 32768.0f;
                wave[i] = v;
                const float a = v < 0.0f ? -v : v;
                if (a > peak)
                    peak = a;
            }
            // Threshold is far below the quietest real signal (attenuation
            // $E, the softest audible step, still peaks around 0.002), so
            // only a genuinely silent channel stays a flat line instead of
            // having its noise floor amplified into a fake waveform.
            const float gain = (peak > 0.0005f) ? (0.92f / peak) : 1.0f;

            int trigger = 0;
            const int half_window = 100;
            for (long i = 1; i < count; i++)
            {
                if (wave[i - 1] < 0.0f && wave[i] >= 0.0f)
                {
                    trigger = (int)i;
                    break;
                }
            }
            int x_min = trigger - half_window;
            int x_max = trigger + half_window;
            if (x_min < 0) x_min = 0;
            if (x_max > count) x_max = (int)count;

            for (long i = 0; i < count; i++)
                wave[i] *= gain;

            ImPlot::SetupAxesLimits(x_min, x_max, -1.11, 1.11, ImPlotCond_Always);
            ImPlot::SetNextLineStyle(green, 1.0f);
            ImPlot::PlotLine("wave", wave, (int)count);
        }
        else
        {
            ImPlot::SetupAxesLimits(0, 1, -1.11, 1.11, ImPlotCond_Always);
        }

        ImPlot::EndPlot();
    }
    ImGui::PopID();
}

#if GEARSF7000_ENABLE_AY
// Same zero-crossing trigger as draw_psg_channel_waveform, reading the AY's
// own debug tap instead of the PSG's.
void draw_ay_channel_waveform(Audio* audio, int channel, float width, float height)
{
    const blip_sample_t* raw = audio->GetAyDebugSamples(channel);
    long count = audio->GetAyDebugSampleCount(channel);

    ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(1, 1));
    const ImPlotAxisFlags flags = ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_NoTickLabels |
        ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoHighlight | ImPlotAxisFlags_Lock |
        ImPlotAxisFlags_NoTickMarks;

    ImGui::PushID(channel + 100);   // +100: shares no cell with the PSG plots
    if (ImPlot::BeginPlot("##ay_wave", ImVec2(width, height), ImPlotFlags_CanvasOnly))
    {
        ImPlot::SetupAxes("x", "y", flags, flags);

        if (raw && count >= 2)
        {
            static float wave_buffer[3][4096];
            float* wave = wave_buffer[channel];
            if (count > 4096)
                count = 4096;

            // Same per-channel normalisation as the PSG plot, and for the
            // same reason: the shape (duty, envelope stepping, noise
            // texture) is what the plot is for, not relative loudness -
            // the Vol column already states the level.
            float peak = 0.0f;
            for (long i = 0; i < count; i++)
            {
                const float v = (float)raw[i] / 32768.0f;
                wave[i] = v;
                const float a = v < 0.0f ? -v : v;
                if (a > peak)
                    peak = a;
            }
            const float gain = (peak > 0.0005f) ? (0.92f / peak) : 1.0f;

            int trigger = 0;
            const int half_window = 100;
            for (long i = 1; i < count; i++)
            {
                if (wave[i - 1] < 0.0f && wave[i] >= 0.0f)
                {
                    trigger = (int)i;
                    break;
                }
            }
            int x_min = trigger - half_window;
            int x_max = trigger + half_window;
            if (x_min < 0) x_min = 0;
            if (x_max > count) x_max = (int)count;

            for (long i = 0; i < count; i++)
                wave[i] *= gain;

            ImPlot::SetupAxesLimits(x_min, x_max, -1.11, 1.11, ImPlotCond_Always);
            ImPlot::SetNextLineStyle(green, 1.0f);
            ImPlot::PlotLine("wave", wave, (int)count);
        }
        else
        {
            ImPlot::SetupAxesLimits(0, 1, -1.11, 1.11, ImPlotCond_Always);
        }

        ImPlot::EndPlot();
    }
    ImGui::PopID();
}
#endif

}

void gui_debug_psg_window(void)
{
    ImGui::SetNextWindowPos(ImVec2(160, 350), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(500, 150), ImGuiCond_FirstUseEver);

    GearSF7000Core* core = emu_get_core();
    Audio* audio = core->GetAudio();

    int periods[5];
    unsigned char volumes[4];

    bool chn_on[4];
    bool chn_mute[4];
    for (int channel = 0; channel < 4; ++channel)
    {
        chn_on[channel] = audio->IsPsgChannelEnabled(channel);
        chn_mute[channel] = audio->IsPsgChannelMuted(channel);
    }

    audio->GetApu()->GetRegs(periods, volumes);

    ImGui::Begin("PSG", &config_debug.show_psg);

    ImGui::PushFont(gui_default_font, gui_get_default_font_size());

    // One row per channel, not one column: a waveform plot (C8) needs
    // horizontal room to be readable, which four narrow table columns side
    // by side would not give it. Enable/mute/period/volume for a channel
    // all sit on that channel's own row, with the waveform cell joining
    // them here once ImPlot is vendored.
    static const char* const kChannelNames[4] = { "Tone 1", "Tone 2", "Tone 3", "Noise" };
    if (ImGui::BeginTable("##psg_channels", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        // Fixed widths sized to the header text, not to a default share of
        // the window: everything these columns hold is one or two glyphs
        // wide, and every pixel they give up goes to the waveform, which is
        // the only cell that gets better the wider it is. "Att", not "Vol":
        // the SN76489 register is attenuation, $0 loudest and $F silent, and
        // that raw register value is what this column prints.
        const float glyph = ImGui::CalcTextSize("0").x;
        ImGui::TableSetupColumn("Channel", ImGuiTableColumnFlags_WidthFixed, glyph * 8.0f);
        ImGui::TableSetupColumn("Ena", ImGuiTableColumnFlags_WidthFixed, glyph * 4.0f);
        ImGui::TableSetupColumn("Mute", ImGuiTableColumnFlags_WidthFixed, glyph * 5.0f);
        ImGui::TableSetupColumn("Period", ImGuiTableColumnFlags_WidthFixed, glyph * 7.0f);
        ImGui::TableSetupColumn("Att", ImGuiTableColumnFlags_WidthFixed, glyph * 4.0f);
        ImGui::TableSetupColumn("Waveform", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        // One row per channel, drawn taller than the text needs, rather than
        // two sub-rows with a hand-placed rowspan across them. That was
        // tried and reverted: ImGui::Table clips each cell against the
        // rectangle of the last TableSetColumnIndex/TableNextRow, so moving
        // the cursor back up to a previous row's column paints the plot into
        // the wrong clip rect and it vanishes entirely.
        const float row_height = 44.0f;

        for (int c = 0; c < 4; c++)
        {
            ImGui::PushID(c);
            ImGui::TableNextRow(ImGuiTableRowFlags_None, row_height);

            const float text_height = ImGui::GetTextLineHeight();
            const float frame_height = ImGui::GetFrameHeight();

            ImGui::TableSetColumnIndex(0);
            center_in_row(row_height, text_height);
            ImGui::TextColored(magenta, "%s", kChannelNames[c]);

            char value[16];

            ImGui::TableSetColumnIndex(1);
            center_in_row(row_height, frame_height);
            center_in_cell(frame_height); // a checkbox is square
            if (ImGui::Checkbox("##on", &chn_on[c]))
                audio->SetPsgChannelEnabled(c, chn_on[c]);

            ImGui::TableSetColumnIndex(2);
            center_in_row(row_height, frame_height);
            center_in_cell(frame_height);
            if (ImGui::Checkbox("##mute", &chn_mute[c]))
                audio->SetPsgChannelMuted(c, chn_mute[c]);

            ImGui::TableSetColumnIndex(3);
            snprintf(value, sizeof(value), "$%04X", periods[c]);
            center_text_in_cell(row_height, value);
            ImGui::TextColored(cyan, "%s", value);

            ImGui::TableSetColumnIndex(4);
            snprintf(value, sizeof(value), "$%01X", volumes[c]);
            center_text_in_cell(row_height, value);
            ImGui::TextColored(cyan, "%s", value);

            ImGui::TableSetColumnIndex(5);
            // Explicit width instead of -1: the plot would otherwise run
            // flush against the cell's right edge, with no gap before the
            // table border.
            const float wave_height = row_height - 8.0f;
            center_in_row(row_height, wave_height);
            draw_psg_channel_waveform(audio, c,
                ImGui::GetContentRegionAvail().x - 4.0f, wave_height);

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    ImGui::TextColored(cyan, "OUT SPEC: "); ImGui::SameLine();
    ImGui::Text("$%01X", periods[4]);

    // Mixer/queue diagnostics, not chip state - closed by default so reading
    // a channel's registers is not fighting for space with CoreAudio ring
    // stats nobody needs open every time the panel is.
    if (ImGui::CollapsingHeader("Diagnostics"))
    {
        const Audio::PeakInfo& peaks = audio->GetPeakInfo();
        ImGui::TextColored(cyan, "MIXER PEAKS: PSG %.0f  DRIVE %.0f  TAPE %.0f", peaks.psg, peaks.drive, peaks.cassette);
        ImGui::TextColored(cyan, "MASTER: pre %.0f  out %.0f  soft-limit samples %u", peaks.preLimiter, peaks.output, peaks.softLimitSamples);
        ImGui::TextColored(cyan, "FRAME SAMPLES: PSG %d  DRIVE %d  TAPE %d",
            peaks.psgFrameSamples, peaks.driveFrameSamples, peaks.cassetteFrameSamples);
        ImGui::TextColored(cyan, "TAPE MONITOR: %.1f Hz", peaks.cassetteFrequencyHz);
        ImGui::TextColored(cyan, "TAPE EVENTS: pending %d  coalesced %u  overflow %u  rebase %u  invalid %u",
            peaks.cassettePendingEvents, peaks.cassetteCoalescedEvents,
            peaks.cassetteQueueOverflows, peaks.cassetteTimestampRebases,
            peaks.cassetteInvalidInputs);
        ImGui::TextColored(cyan, "QUEUED: DRIVE %d  TAPE %d",
            peaks.driveQueuedSamples, peaks.cassetteQueuedSamples);
        const EmuAudioQueueDiagnostics queueDiagnostics = emu_get_audio_queue_diagnostics();
        if (queueDiagnostics.callbacks > 0)
        {
            ImGui::TextColored(cyan,
                "COREAUDIO: %llu -> %llu Hz  cb %llu  req %llu..%llu  variable %llu",
                queueDiagnostics.client_sample_rate, queueDiagnostics.device_sample_rate,
                queueDiagnostics.callbacks, queueDiagnostics.min_requested_samples,
                queueDiagnostics.max_requested_samples,
                queueDiagnostics.variable_size_callbacks);
            ImGui::TextColored(cyan,
                "COREAUDIO PERIOD: %llu..%llu us",
                queueDiagnostics.min_callback_interval_us,
                queueDiagnostics.max_callback_interval_us);
            ImGui::TextColored(cyan,
                "RING: %llu/%llu  underruns %llu  zero-fill %llu  dropped %llu",
                queueDiagnostics.queued_samples, queueDiagnostics.ring_capacity_samples,
                queueDiagnostics.underruns, queueDiagnostics.zero_filled_samples,
                queueDiagnostics.producer_dropped_samples);
        }
        if (ImGui::Button("Reset audio queue stats"))
            emu_reset_audio_queue_diagnostics();
        if (ImGui::Button("Reset mixer peaks"))
            audio->ResetPeakInfo();
    }

    ImGui::PopFont();

    ImGui::End();
}

#if GEARSF7000_ENABLE_AY
void gui_debug_ay_window(void)
{
    ImGui::SetNextWindowPos(ImVec2(180, 380), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(500, 260), ImGuiCond_FirstUseEver);

    Audio* audio = emu_get_core()->GetAudio();

    ImGui::Begin("AY-3-8910 / YM2149", &config_debug.show_ay);
    ImGui::PushFont(gui_default_font, gui_get_default_font_size());

    if (!audio->IsAyEnabled())
    {
        // Deliberately says why rather than showing an empty grid: the card is
        // staged in the Audio menu and only latched on reset, so "off" here
        // usually means "enabled but not reset yet".
        ImGui::TextColored(magenta, "EXPANSION NOT PRESENT");
        ImGui::TextColored(cyan, "Enable it in Audio > AY Expansion, then reset.");
        ImGui::PopFont();
        ImGui::End();
        return;
    }

    Ay_Apu* ay = audio->GetAyApu();
    const bool ym = audio->GetAyChip() == Audio::AyChip::YM2149;
    const int clockHz = audio->GetAyClockHz();

    int periods[3];
    unsigned char volumes[3];
    ay->GetRegs(periods, volumes);
    const u8 mixer = (u8)ay->read(7);

    bool chn_on[3];
    bool chn_mute[3];
    for (int channel = 0; channel < 3; ++channel)
    {
        chn_on[channel] = audio->IsAyChannelEnabled(channel);
        chn_mute[channel] = audio->IsAyChannelMuted(channel);
    }

    ImGui::TextColored(magenta, "%s", ym ? "YM2149" : "AY-3-8910"); ImGui::SameLine();
    ImGui::TextColored(cyan, "  CLOCK %d Hz   PORTS $%02X-$%02X   LATCHED R%d",
        clockHz, GetAyPortBase(), GetAyPortBase() + 3, audio->GetAySelectedRegister());

    ImGui::Separator();

    // Same table shape as the PSG panel: one row per channel so the
    // waveform gets horizontal room, not one column per channel. Two extra
    // columns the PSG has no equivalent for - Mix, the per-channel tone/
    // noise mixer bits - because unlike the PSG's dedicated noise channel,
    // each AY channel independently mixes tone and/or noise from the same
    // shared generators (see EnableChannel's comment on R7 being shared).
    static const char* const kChannelNames[3] = { "Chn A", "Chn B", "Chn C" };
    if (ImGui::BeginTable("##ay_channels", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
    {
        const float glyph = ImGui::CalcTextSize("0").x;
        ImGui::TableSetupColumn("Channel", ImGuiTableColumnFlags_WidthFixed, glyph * 8.0f);
        ImGui::TableSetupColumn("Ena", ImGuiTableColumnFlags_WidthFixed, glyph * 4.0f);
        ImGui::TableSetupColumn("Mute", ImGuiTableColumnFlags_WidthFixed, glyph * 5.0f);
        ImGui::TableSetupColumn("Period", ImGuiTableColumnFlags_WidthFixed, glyph * 7.0f);
        ImGui::TableSetupColumn("Vol", ImGuiTableColumnFlags_WidthFixed, glyph * 5.0f);
        ImGui::TableSetupColumn("Mix", ImGuiTableColumnFlags_WidthFixed, glyph * 4.0f);
        ImGui::TableSetupColumn("Waveform", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        const float row_height = 44.0f;

        for (int c = 0; c < 3; c++)
        {
            ImGui::PushID(c);
            ImGui::TableNextRow(ImGuiTableRowFlags_None, row_height);

            const float text_height = ImGui::GetTextLineHeight();
            const float frame_height = ImGui::GetFrameHeight();

            ImGui::TableSetColumnIndex(0);
            center_in_row(row_height, text_height);
            ImGui::TextColored(magenta, "%s", kChannelNames[c]);

            char value[16];

            ImGui::TableSetColumnIndex(1);
            center_in_row(row_height, frame_height);
            center_in_cell(frame_height);
            if (ImGui::Checkbox("##on", &chn_on[c]))
                audio->SetAyChannelEnabled(c, chn_on[c]);

            ImGui::TableSetColumnIndex(2);
            center_in_row(row_height, frame_height);
            center_in_cell(frame_height);
            if (ImGui::Checkbox("##mute", &chn_mute[c]))
                audio->SetAyChannelMuted(c, chn_mute[c]);

            ImGui::TableSetColumnIndex(3);
            snprintf(value, sizeof(value), "$%03X", periods[c]);
            center_text_in_cell(row_height, value);
            ImGui::TextColored(cyan, "%s", value);

            ImGui::TableSetColumnIndex(4);
            // Volume is either a fixed 4-bit level or "envelope controls
            // this channel" - bit 4 of R8+c, same convention the old panel
            // used. Not attenuation like the PSG's register: this one is
            // already loudest-at-max.
            if (volumes[c] & 0x10)
                snprintf(value, sizeof(value), "ENV");
            else
                snprintf(value, sizeof(value), "$%01X", volumes[c] & 0x0F);
            center_text_in_cell(row_height, value);
            ImGui::TextColored(cyan, "%s", value);

            ImGui::TableSetColumnIndex(5);
            // Mixer bits are active-low: a set bit disables that source.
            // T/N shown only when actually enabled, dash otherwise, so the
            // column reads at a glance instead of needing "on"/"off" spelled
            // out in a 4-glyph cell.
            snprintf(value, sizeof(value), "%c%c",
                (mixer & (1 << c)) ? '-' : 'T',
                (mixer & (1 << (c + 3))) ? '-' : 'N');
            center_text_in_cell(row_height, value);
            ImGui::TextColored(cyan, "%s", value);

            ImGui::TableSetColumnIndex(6);
            const float wave_height = row_height - 8.0f;
            center_in_row(row_height, wave_height);
            draw_ay_channel_waveform(audio, c,
                ImGui::GetContentRegionAvail().x - 4.0f, wave_height);

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    // Shared resources, deliberately kept out of the table: unlike period/
    // volume/mixer, these belong to no single channel - one noise generator
    // and one envelope generator serve whichever channels' mixer/volume bits
    // opt into them (see EnableChannel's comment on R6/R11-13).
    const int noisePeriod = (int)ay->read(6) & 0x1F;
    ImGui::TextColored(cyan, "NOISE PERIOD: "); ImGui::SameLine();
    ImGui::Text("$%02X  (%.0f Hz)", noisePeriod,
        noisePeriod ? (double)clockHz / (32.0 * noisePeriod) : 0.0);
    ImGui::SameLine();

    const int envPeriod = ((int)ay->read(12) << 8) | (int)ay->read(11);
    ImGui::TextColored(cyan, "   ENVELOPE: "); ImGui::SameLine();
    ImGui::Text("period $%04X  shape $%01X  %d steps", envPeriod,
        (int)ay->read(13) & 0x0F, ym ? 32 : 16);

    // Raw register file - reference only, closed by default like the PSG
    // panel's mixer/queue diagnostics: reading a channel's decoded state in
    // the table above is the common case, not fighting for space with this.
    if (ImGui::CollapsingHeader("Registers"))
    {
        for (int row = 0; row < 2; row++)
        {
            ImGui::TextColored(magenta, "R%X-R%X: ", row * 8, row * 8 + 7); ImGui::SameLine();
            for (int i = 0; i < 8; i++)
            {
                ImGui::Text("%02X ", (u8)ay->read(row * 8 + i));
                if (i < 7) ImGui::SameLine();
            }
        }
    }

    ImGui::PopFont();
    ImGui::End();
}
#endif
