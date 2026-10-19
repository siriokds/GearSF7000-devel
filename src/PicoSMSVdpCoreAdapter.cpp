/* GearSF7000 - picoSMSvdp-core scanline backend */
#include "Pico9918LegacyCore.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <pico9918.h>
#include <pico9918_debug.h>
#include <pico9918_emulator.h>
#include <pico9918_frame.h>

namespace {
class PicoSMSBackend final : public PicoVdpCoreBackend {
public:
    explicit PicoSMSBackend(bool smsHost) : core(pico9918_new()) {
        // The board the core sits on: the Mark III bus says SMS, a TMS socket not.
        if (core) pico9918_set_sms_host(core, smsHost);
        if (!core) return;
        pico9918_set_chip(core, PICO9918_CHIP_PICO9918);
        pico9918_reset(core);
        // Keep the core's reset palette and mapped control/status windows;
        // only guest VRAM receives GearSF7000's deterministic power-on fill.
        std::array<std::uint8_t, 0x4000> clear{};
        pico9918_debug_write(core, 0, clear.data(), clear.size());
    }
    ~PicoSMSBackend() override { if (core) pico9918_destroy(core); }
    bool IsValid() const override { return core != nullptr; }
    void Reset() override { if (core) pico9918_reset(core); latchPending = false; latchValue = 0; accessCode = 0; }
    void WriteControl(std::uint8_t v) override {
        if (!core) return;
        pico9918_write_addr(core, v);
        if (latchPending) { latchPending = false; accessCode = static_cast<std::uint8_t>((v >> 6) & 3); }
        else { latchPending = true; latchValue = v; }
    }
    void WriteData(std::uint8_t v) override { if (core) pico9918_write_data(core, v); latchPending = false; }
    std::uint8_t ReadData() override { latchPending = false; return core ? pico9918_read_data(core) : 0; }
    std::uint8_t ReadStatus() override { latchPending = false; return core ? pico9918_read_status(core) : 0; }
    bool InterruptAsserted() const override { return core && pico9918_interrupt_status(core); }
    void RenderScanline(std::size_t line, std::uint8_t* pixels, bool row30,
        bool realY, std::uint8_t tilePal, std::uint8_t spritePal,
        std::uint8_t lineSprites, std::uint8_t sprites, std::uint8_t tileEcm,
        std::uint16_t tileOffset, std::uint8_t spriteEcm,
        std::uint16_t spriteOffset, std::uint8_t* background,
        std::uint8_t* spriteMask) override {
        if (!core || !pixels) return;
        // These compatibility arguments belong to GearSF7000's legacy
        // Pico9918 wrapper. This backend already owns its complete register
        // file, so rewriting five debug registers on every scanline is both
        // incorrect and needlessly expensive.
        (void)row30; (void)realY; (void)tilePal; (void)spritePal;
        (void)lineSprites; (void)sprites; (void)tileEcm; (void)tileOffset;
        (void)spriteEcm; (void)spriteOffset;
        pico9918_frame_update_interrupts(core, pico9918_scan_line(core, static_cast<std::uint16_t>(line)));
        const std::uint8_t* source = pico9918_line_source(core);
        if (source) std::memcpy(pixels, source, 256);
        if (background) std::memcpy(background, pixels, 256);
        if (spriteMask) std::memset(spriteMask, 0, 256);
    }
    void EndActiveDisplay() override {
        if (core) pico9918_frame_sms_post_active_line(core);
    }
    PicoVdpCoreMode GetMode() const override {
        if (!core) return PicoVdpCoreMode::GraphicsI;
        const std::uint8_t r0 = pico9918_debug_reg(core, 0);
        const std::uint8_t r1 = pico9918_debug_reg(core, 1);
        if (r0 & 0x04) return PicoVdpCoreMode::SmsMode4;
        if (r1 & 0x10) return PicoVdpCoreMode::Text;
        if (r1 & 0x08) return PicoVdpCoreMode::Multicolor;
        if (r0 & 0x02) return PicoVdpCoreMode::GraphicsII;
        return PicoVdpCoreMode::GraphicsI;
    }
    std::uint16_t ActiveLineCount() const override {
        if (!core) return 192;
        const std::uint8_t r0 = pico9918_debug_reg(core, 0);
        const std::uint8_t r1 = pico9918_debug_reg(core, 1);
        if ((r0 & 0x06) != 0x06) return 192;
        const bool lines224 = (r1 & 0x10) != 0;
        const bool lines240 = (r1 & 0x08) != 0;
        return lines224 == lines240 ? 192 : (lines224 ? 224 : 240);
    }
    std::uint16_t ReadPalette(std::size_t i) const override { return core && i < 64 ? pico9918_debug_palette(core, static_cast<std::uint8_t>(i)) : 0; }
    bool IsPaletteWriteActive() const override {
        return core && (accessCode == 3 ||
            (pico9918_debug_reg(core, 47) & 0x80) != 0);
    }
    std::uint8_t ReadRegister(std::size_t i) const override { return core && i < 64 ? pico9918_debug_reg(core, static_cast<std::uint8_t>(i)) : 0; }
    void WriteRegister(std::size_t i, std::uint8_t v) override { if (core && i < 64) pico9918_debug_reg_write(core, static_cast<std::uint8_t>(i), v); }
    void WriteStatus(std::uint8_t v) override { if (core) pico9918_set_status(core, v); }
    void AdjustAddress(std::int16_t) override {}
    std::uint8_t ReadVram(std::size_t a) const override {
        std::uint8_t v = 0;
        if (core && a < 0x10000) pico9918_debug_read(core, static_cast<std::uint32_t>(a), &v, 1);
        return v;
    }
    bool WriteVram(std::size_t a, std::uint8_t v) override { return core && a < 0x10000 && pico9918_debug_write(core, static_cast<std::uint32_t>(a), &v, 1) == 1; }
    void CancelControlLatch() override {
        if (core && latchPending) {
            const std::uint8_t s = pico9918_peek_status(core);
            pico9918_read_status(core); pico9918_set_status(core, s);
        }
        latchPending = false;
    }
    bool ReadDebugState(PicoVdpCoreDebugState& s) const override {
        if (!core) return false;
        s = {pico9918_debug_vram_address(core), pico9918_peek_status(core),
            latchPending, latchValue, pico9918_read_data_no_inc(core)};
        return true;
    }
    std::size_t StateSize() const override { return pico9918_emulator_state_size() + 3; }
    void SaveState(void* destination) const override {
        if (!core || !destination) return;
        auto* p = static_cast<std::uint8_t*>(destination);
        p[0] = latchPending ? 1 : 0; p[1] = latchValue; p[2] = accessCode;
        pico9918_emulator_save_state(core, p + 3, pico9918_emulator_state_size());
    }
    bool LoadState(const void* source, std::size_t size) override {
        // The core decides which sizes it takes: it still loads a format-1 state,
        // smaller than StateSize() (pico9918_emulator_load_state).
        if (!core || !source || size < 3) return false;
        const auto* p = static_cast<const std::uint8_t*>(source);
        if (p[0] > 1 || p[2] > 3 ||
            !pico9918_emulator_load_state(core, p + 3, size - 3)) return false;
        latchPending = p[0] != 0; latchValue = p[1]; accessCode = p[2];
        return true;
    }
    bool SupportsSmsCounters() const override { return core != nullptr; }
    void SetGameGearCram(bool enabled) override {
        if (core) pico9918_set_gg_cram(core, enabled);
    }
    void ConfigureSmsRaster(bool pal) override {
        if (core) pico9918_sms_set_pal(core, pal);
    }
    void LatchSmsScroll(std::uint16_t line) override {
        if (core) pico9918_sms_latch_scroll(core, line);
    }
    void ClockSmsLineCounter(std::uint16_t line) override {
        if (core) pico9918_sms_line_counter_event(core, line);
    }
    std::uint8_t ReadSmsLatchedHScroll() const override {
        return core ? pico9918_sms_latched_hscroll(core) : 0;
    }
    std::uint8_t ReadSmsLatchedVScroll() const override {
        return core ? pico9918_sms_latched_vscroll(core) : 0;
    }
    void SetSmsRasterPosition(std::uint16_t line,
                              std::uint16_t column) override {
        if (core) pico9918_sms_set_raster_position(core, line, column);
    }
    std::uint8_t ReadSmsVCounter() const override {
        return core ? pico9918_sms_v_counter(core) : 0xFF;
    }
    void LatchSmsHCounter() override {
        if (core) pico9918_sms_latch_h_counter(core);
    }
    std::uint8_t ReadSmsHCounter() const override {
        return core ? pico9918_sms_h_counter(core) : 0xFF;
    }
private:
    pico9918_t* core = nullptr;
    bool latchPending = false;
    std::uint8_t latchValue = 0;
    std::uint8_t accessCode = 0;
};
}

std::unique_ptr<PicoVdpCoreBackend> CreatePicoSMSVdpBackend(bool smsHost)
{
    return std::make_unique<PicoSMSBackend>(smsHost);
}
