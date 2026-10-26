// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
/* Internal scanline-backend contract shared by Pico9918 and PicoSMSvdp. */
#ifndef PICO_VDP_CORE_BACKEND_H
#define PICO_VDP_CORE_BACKEND_H

#include <cstddef>
#include <cstdint>
#include <memory>

enum class PicoVdpCoreMode : std::uint8_t
{
    GraphicsI,
    GraphicsII,
    Text,
    Multicolor,
    SmsMode4
};

struct PicoVdpCoreDebugState
{
    std::uint16_t address = 0;
    std::uint8_t status = 0;
    bool controlLatchPending = false;
    std::uint8_t controlLatchValue = 0;
    std::uint8_t readAheadBuffer = 0;
};

class PicoVdpCoreBackend
{
public:
    virtual ~PicoVdpCoreBackend() = default;
    virtual bool IsValid() const = 0;
    virtual void Reset() = 0;
    virtual void WriteControl(std::uint8_t value) = 0;
    virtual void WriteData(std::uint8_t value) = 0;
    virtual std::uint8_t ReadData() = 0;
    virtual std::uint8_t ReadStatus() = 0;
    virtual bool InterruptAsserted() const = 0;
    virtual void RenderScanline(std::size_t line,
        std::uint8_t* colorIndexes, bool row30, bool realSpriteY,
        std::uint8_t tilePaletteBase, std::uint8_t spritePaletteBase,
        std::uint8_t maxScanlineSprites, std::uint8_t maxSprites,
        std::uint8_t tileEcm, std::uint16_t tileEcmOffset,
        std::uint8_t spriteEcm, std::uint16_t spriteEcmOffset,
        std::uint8_t* debugBackground,
        std::uint8_t* debugSpriteMask) = 0;
    virtual void EndActiveDisplay() = 0;
    virtual PicoVdpCoreMode GetMode() const = 0;
    virtual std::uint16_t ActiveLineCount() const = 0;
    virtual std::uint16_t ReadPalette(std::size_t index) const = 0;
    virtual bool IsPaletteWriteActive() const = 0;
    virtual std::uint8_t ReadRegister(std::size_t index) const = 0;
    virtual void WriteRegister(std::size_t index, std::uint8_t value) = 0;
    virtual void WriteStatus(std::uint8_t value) = 0;
    virtual void AdjustAddress(std::int16_t delta) = 0;
    virtual std::uint8_t ReadVram(std::size_t address) const = 0;
    virtual bool WriteVram(std::size_t address, std::uint8_t value) = 0;
    virtual void CancelControlLatch() = 0;
    virtual bool ReadDebugState(PicoVdpCoreDebugState& state) const = 0;
    virtual std::size_t StateSize() const = 0;
    virtual void SaveState(void* destination) const = 0;
    virtual bool LoadState(const void* source, std::size_t size) = 0;
    virtual bool SupportsSmsCounters() const { return false; }
    virtual void SetGameGearCram(bool enabled) { (void)enabled; }
    virtual void ConfigureSmsRaster(bool pal) { (void)pal; }
    virtual void LatchSmsScroll(std::uint16_t line) { (void)line; }
    virtual void ClockSmsLineCounter(std::uint16_t line) { (void)line; }
    virtual std::uint8_t ReadSmsLatchedHScroll() const { return 0; }
    virtual std::uint8_t ReadSmsLatchedVScroll() const { return 0; }
    virtual void SetSmsRasterPosition(std::uint16_t line,
                                      std::uint16_t column)
    {
        (void)line;
        (void)column;
    }
    virtual std::uint8_t ReadSmsVCounter() const { return 0xFF; }
    virtual void LatchSmsHCounter() {}
    virtual std::uint8_t ReadSmsHCounter() const { return 0xFF; }
};

std::unique_ptr<PicoVdpCoreBackend> CreatePico9918LegacyBackend();
#if GEARSF7000_USE_PICOSMSVDP_CORE
std::unique_ptr<PicoVdpCoreBackend> CreatePicoSMSVdpBackend();
#endif

#endif
