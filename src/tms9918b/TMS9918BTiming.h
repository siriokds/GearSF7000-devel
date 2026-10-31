/*
 * TMS9918B reference model - TMS9918B VRAM cycle timing
 *
 * Copyright 2026 Saverio Russo
 * SPDX-License-Identifier: Apache-2.0
 *
 * The TMS9918B is a hypothetical 1983 revision of the TMS9918A for the same
 * socket, the same crystal and the same VRAM board (8 x 16K x 1 dynamic RAM,
 * e.g. the Fujitsu MB8118-12 found on 1983 Sega SC-3000 boards).
 *
 * Time base: one "unit" is half a period of the 10.738635 MHz crystal
 * (about 46.56 ns). The TMS9918A clock input is specified as two phases
 * (XTAL1/XTAL2, 42-52 ns high and low, 42-52 ns apart), so both edges are
 * available on the chip. A TMS99xx phase is 2 units, a TMS99xx slot 8 units,
 * a line 1368 units, a Z80 T-state at 3.58 MHz 6 units.
 *
 * Every VRAM access is one RAS cycle reading 1 to 4 adjacent bytes in page
 * mode. Waveform of an n-byte cycle starting at unit 0:
 *
 *   RAS  low  [0, 5n-1)          high (precharge) [5n-1, 5n+2)
 *   CAS  byte k low [1+5k, 4+5k) sample on the rising edge at 4+5k
 *   WE   (single-byte writes, late write) low [2, 4): AD0-AD7 carry the row
 *        address at RAS, the column address at CAS and then the data, which
 *        the RAM latches on the falling edge of WE
 *   length 5n+2 units: 1 byte 7, 2 bytes 12, 3 bytes 17, 4 bytes 22
 *
 * TMS modes run a 1-byte cycle at the start of every TMS99xx slot (7 units
 * active + 1 idle), so the CPU access windows are the TMS9918A ones.
 */

#ifndef TMS9918B_TIMING_H
#define TMS9918B_TIMING_H

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace TMS9918BModel
{
    constexpr double CrystalHz = 10738635.0;
    constexpr double NanosecondsPerUnit = 1.0e9 / (CrystalHz * 2.0);   // 46.56 ns
    constexpr std::uint16_t UnitsPerPhase  = 2;
    constexpr std::uint16_t UnitsPerSlot   = 8;
    constexpr std::uint16_t UnitsPerLine   = 1368;
    constexpr std::uint16_t UnitsPerTState = 6;
    constexpr std::uint16_t PortLatencyUnits = 44;      // 22 phases, about 2 us

    // Design rules of the TMS9918B calendars.
    constexpr std::uint16_t DependencyUnits = 10;       // sample -> RAS of a dependent address (TMS Text: name -> pattern)
    constexpr std::uint16_t PixelLoadLeadUnits = 4;     // last byte of a cell sampled this long before its first pixel

    // VDP-side interface assumptions (Appendix B style budget).
    constexpr double VdpDataSetupNs   = 40.0;           // RD bus setup before CAS rising edge (TMS9918A: 60 ns)
    constexpr double SystemDelayNs    = 20.0;           // board: VDP -> RAM strobes + RAM -> VDP data

    constexpr std::uint16_t CycleUnits(std::uint8_t bytes) { return static_cast<std::uint16_t>(5u * bytes + 2u); }
    constexpr std::uint16_t SampleOffset(std::uint8_t byteIndex) { return static_cast<std::uint16_t>(4u + 5u * byteIndex); }
    constexpr double Ns(double units) { return units * NanosecondsPerUnit; }

    // ----- dynamic RAM data sheet limits (ns, minimum unless noted) -------------
    struct DramSpec
    {
        const char* name;
        double tRC, tRAC, tCAC, tRP, tRAS, tRPM, tRSH, tCSH, tCAS, tCPN, tCP, tPC;
        double tRCDmin, tRCDmax, tAR, tRAH, tASC, tCAH;
        double tWCH, tWCR, tWP, tRWL, tCWL, tDH, tDHR;
        bool pageMode;
    };

    // Fujitsu MB8118-12 data sheet (page mode, hidden refresh, single +5 V).
    constexpr DramSpec MB8118_12  {"Fujitsu MB8118-12",   270, 120, 65, 120, 140, 140,  85, 120,  65, 55, 70, 145, 25, 55, 70, 15, 0, 15, 35,  90, 35,  65,  50, 35,  90, true};
    // Motorola MCM4517 data sheet, -12 and -15 columns.
    constexpr DramSpec MCM4517_12 {"Motorola MCM4517-12", 270, 120, 65, 120, 140, 140,  85, 120,  65, 55, 70, 145, 25, 55, 70, 15, 0, 15, 30,  85, 30,  65,  50, 30,  85, true};
    constexpr DramSpec MCM4517_15 {"Motorola MCM4517-15", 320, 150, 80, 135, 175, 175, 105, 165,  95, 70, 85, 190, 25, 70, 90, 20, 0, 20, 45, 115, 50, 110, 100, 45, 115, true};
    // TI TMS4116-20, reference only (no page mode specification used here).
    constexpr DramSpec TMS4116_20 {"TI TMS4116-20",       375, 200, 135, 120, 200, 200,  0,   0, 135,  0,  80,   0, 20, 65,  0,  0, 0,  0,  0,   0,  0,   0,   0,  0,   0, false};

    struct Check
    {
        std::string parameter;
        double required;    // ns
        double actual;      // ns
        bool Ok() const { return actual + 1e-9 >= required; }
        double Margin() const { return actual - required; }
    };

    // Read cycle of 'bytes' adjacent bytes.
    inline std::vector<Check> VerifyRead(const DramSpec& d, std::uint8_t bytes)
    {
        const double n = bytes;
        std::vector<Check> c;
        c.push_back({"tRC cycle time", d.tRC, Ns(5 * n + 2)});
        c.push_back({bytes > 1 ? "tRPM RAS low (page mode)" : "tRAS RAS low", bytes > 1 ? d.tRPM : d.tRAS, Ns(5 * n - 1)});
        c.push_back({"tRP RAS precharge", d.tRP, Ns(3)});
        c.push_back({"tRCD RAS to CAS delay (min)", d.tRCDmin, Ns(1)});
        c.push_back({"tRCD RAS to CAS delay (max, tRAC applies)", Ns(1), d.tRCDmax});
        c.push_back({"tCAS CAS low", d.tCAS, Ns(3)});
        c.push_back({"tCSH CAS hold after RAS fall", d.tCSH, Ns(4)});
        c.push_back({"tRSH RAS hold after last CAS fall", d.tRSH, Ns(3)});
        c.push_back({"tCPN CAS precharge before next cycle", d.tCPN, Ns(4)});
        c.push_back({"tAR column address hold from RAS", d.tAR, Ns(4)});
        c.push_back({"tCAH column address hold", d.tCAH, Ns(3)});
        c.push_back({"tRAH + tASC within tRCD", d.tRAH + d.tASC, Ns(1)});
        if (bytes > 1)
        {
            c.push_back({"tCP CAS precharge (page mode)", d.tCP, Ns(2)});
            c.push_back({"tPC page mode cycle", d.tPC, Ns(5)});
        }
        const double first = std::max(d.tRAC, Ns(1) + d.tCAC) + SystemDelayNs + VdpDataSetupNs;
        c.push_back({"data byte 0: access + delays + VDP setup", first, Ns(4)});
        if (bytes > 1)
            c.push_back({"data byte k>0: tCAC + delays + VDP setup", d.tCAC + SystemDelayNs + VdpDataSetupNs, Ns(3)});
        return c;
    }

    // Single-byte late-write cycle (CPU writes). The AD bus is shared by
    // address and data, so data is presented after the column address hold and
    // latched by the WE falling edge at unit 2.
    constexpr double VdpBusSwitchNs = 10.0;   // AD multiplexer, column address to data

    inline std::vector<Check> VerifyWrite(const DramSpec& d)
    {
        std::vector<Check> c = VerifyRead(d, 1);
        c.erase(std::remove_if(c.begin(), c.end(), [](const Check& k) { return k.parameter.rfind("data byte", 0) == 0; }), c.end());
        c.push_back({"column address hold + bus switch before data setup (tCAH)", d.tCAH + VdpBusSwitchNs, Ns(1)});
        c.push_back({"tWCH write hold after CAS fall", d.tWCH, Ns(3)});
        c.push_back({"tWCR write hold from RAS", d.tWCR, Ns(4)});
        c.push_back({"tWP write pulse", d.tWP, Ns(2)});
        c.push_back({"tRWL write to RAS rise", d.tRWL, Ns(2)});
        c.push_back({"tCWL write to CAS rise", d.tCWL, Ns(2)});
        c.push_back({"tDH data hold after WE fall", d.tDH, Ns(2)});
        c.push_back({"tDHR data hold from RAS", d.tDHR, Ns(4)});
        return c;
    }

    inline bool AllOk(const std::vector<Check>& checks)
    {
        return std::all_of(checks.begin(), checks.end(), [](const Check& c) { return c.Ok(); });
    }
}

#endif // TMS9918B_TIMING_H
