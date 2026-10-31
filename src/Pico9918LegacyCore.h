/*
 * GearSF7000 - replaceable vrEmuTms9918 adapter for Pico9918
 * Copyright (C) 2026  Saverio Russo
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef PICO9918_LEGACY_CORE_H
#define PICO9918_LEGACY_CORE_H

#include <cstddef>
#include <cstdint>

#ifndef GEARSF7000_USE_PICOSMSVDP_CORE
#define GEARSF7000_USE_PICOSMSVDP_CORE 0
#endif

#include <memory>

#include "PicoVdpCoreBackend.h"

// This is the only GearSF7000 class allowed to depend on a scanline core API.
// Pico9918 owns one adapter and provides the machine-facing timing, frame,
// debug and save-state contracts. Keeping the boundary narrow makes backend
// replacement a build choice rather than a change throughout the device.
class Pico9918LegacyCore
{
public:
    static constexpr std::size_t Width = 256;
    static constexpr std::size_t Height = 192;
    static constexpr std::size_t ExtendedHeight = 240;
    static constexpr std::size_t VramSize = 1u << 16;

    enum class Backend { Pico9918, PicoSMSVdp };
    using Mode = PicoVdpCoreMode;
    using DebugState = PicoVdpCoreDebugState;

    // smsHost: the PicoSMSVdp core sits on a Mark III / Master System board and answers
    // as the SMS VDP in every mode; otherwise it is in a TMS9918A socket.
    explicit Pico9918LegacyCore(Backend backend = Backend::Pico9918, bool smsHost = false);
    ~Pico9918LegacyCore();

    Pico9918LegacyCore(const Pico9918LegacyCore&) = delete;
    Pico9918LegacyCore& operator=(const Pico9918LegacyCore&) = delete;

    bool IsValid() const;
    bool IsSmsBackend() const { return m_backend == Backend::PicoSMSVdp; }
    void Reset();

    void WriteControl(std::uint8_t value);
    void WriteData(std::uint8_t value);
    std::uint8_t ReadData();
    std::uint8_t ReadStatus();
    bool InterruptAsserted() const;

    void RenderScanline(std::size_t line, std::uint8_t* colorIndexes,
        bool row30 = false, bool realSpriteY = false,
        std::uint8_t tilePaletteBase = 0,
        std::uint8_t spritePaletteBase = 0,
        std::uint8_t maxScanlineSprites = 4,
        std::uint8_t maxSprites = 32, std::uint8_t tileEcm = 0,
        std::uint16_t tileEcmOffset = 0x800,
        std::uint8_t spriteEcm = 0,
        std::uint16_t spriteEcmOffset = 0x800,
        std::uint8_t* debugBackground = nullptr,
        std::uint8_t* debugSpriteMask = nullptr);
    void EndActiveDisplay();
    Mode GetMode() const;
    std::uint16_t ActiveLineCount() const;
    std::uint16_t ReadPalette(std::size_t index) const;
    bool IsPaletteWriteActive() const;
    std::uint8_t ReadRegister(std::size_t index) const;
    void WriteRegister(std::size_t index, std::uint8_t value);
    void WriteStatus(std::uint8_t value);
    void AdjustAddress(std::int16_t delta);
    std::uint8_t ReadVram(std::size_t address) const;
    bool WriteVram(std::size_t address, std::uint8_t value);
    void CancelControlLatch();
    bool ReadDebugState(DebugState& state) const;
    std::size_t StateSize() const;
    void SaveState(void* destination) const;
    bool LoadState(const void* source, std::size_t size);
    bool SupportsSmsCounters() const;
    void SetGameGearCram(bool enabled);
    void ConfigureSmsRaster(bool pal);
    void LatchSmsScroll(std::uint16_t line);
    void ClockSmsLineCounter(std::uint16_t line);
    std::uint8_t ReadSmsLatchedHScroll() const;
    std::uint8_t ReadSmsLatchedVScroll() const;
    void SetSmsRasterPosition(std::uint16_t line, std::uint16_t column);
    std::uint8_t ReadSmsVCounter() const;
    void LatchSmsHCounter();
    std::uint8_t ReadSmsHCounter() const;

private:
    Backend m_backend;
    std::unique_ptr<PicoVdpCoreBackend> m_core;
};

std::unique_ptr<PicoVdpCoreBackend> CreatePico9918LegacyBackend();
#if GEARSF7000_USE_PICOSMSVDP_CORE
std::unique_ptr<PicoVdpCoreBackend> CreatePicoSMSVdpBackend(bool smsHost);
#endif

#endif
