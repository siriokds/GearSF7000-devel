// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SYSTEM_BUS_MSX_H
#define SYSTEM_BUS_MSX_H
#include "ISystemBus.h"
#include "Intel8255.h"
#include "MSXKeyboard.h"
#include "MSXSlotMapper.h"
#if GEARSF7000_ENABLE_SR1000
#include "MSXTape.h"
#endif
#include <memory>
#include <vector>
#include <string>
class Memory;
class Audio;
class MachineIOPorts;
class Cartridge;

// Generic MSX1: BIOS in primary 0, cartridge in 1, empty 2, 64 KiB RAM in 3.
// PAL/NTSC changes the fitted VDP; both boards clock the Z80 at 3.579545 MHz.
class SystemBusMSX final : public ISystemBus, private IIntel8255Pins
{
public:
    SystemBusMSX(Memory& memory, Cartridge& cart, Audio& audio, MachineIOPorts& ports);
    ~SystemBusMSX() override;
    void ConnectPeripherals(); // Only for the installed bus, never state-validation candidates.
#if GEARSF7000_ENABLE_SR1000
    MSXTape& Cassette() { return m_tape; }
    const MSXTape& Cassette() const { return m_tape; }
#endif
    RamRegions GetRamRegions() const override;
    MachineType GetMachineType() const override { return MachineType::MSX; }
    u32 GetCpuPsgClockHz() const override { return 3'579'545; }
    u32 GetBiosSize() const override { return 0x8000; }
    bool HasOneBitAudio() const override { return true; }
    bool KeyboardClickLevel() const { return m_keyboardClick; }
    unsigned GetWaitCyclesM1() const override { return 1; }
    unsigned GetIOWaitStates(u64, u16, bool) const override { return 0; }
    VdpLineType GetVdpLineType() const override { return VdpLineType::VDP_INT; }
    PsgPortDecode GetPsgPorts() const override { return {0xFF, 0xA1}; }
    FirmwareRange GetFirmwareRange() const override { return {0, 0x8000}; }
    SystemInputDescriptor DescribeInput() const override { return {0, false, true, "", ""}; }
    u8 AcknowledgeInterrupt(u64) override { return 0xFF; }
    u8 ReadMemory(u16 address) override;
    void WriteMemory(u16 address, u8 value) override;
    u8 ReadIO(u64 time, u16 address) override;
    void WriteIO(u64 time, u16 address, u8 value) override;
    DebugDeviceDefinition DescribeDebugDevice() const override;
    DebugProbe GetIOProbe(u16 address) const override;
    void BindDebugEvents(const DebugDeviceRegistry& registry);
    struct PageMapping {
        u16 start, end;
        u8 primary, secondary;
        bool expanded, mapped, writable;
        const char* device;
        u32 offset;
    };
    std::array<PageMapping, 4> GetPageMappings() const;
    const MSXSlotMapper& Slots() const { return m_slots; }
    const Intel8255& PPI() const { return m_ppi; }
    u8 KeyboardRow() const { return m_keyboardRow; }
    MSXSlotMapper& Slots() { return m_slots; }
    MSXKeyboard& Keyboard() { return m_keyboard; }
    const MSXKeyboard& Keyboard() const { return m_keyboard; }
    Intel8255& PPI() { return m_ppi; }
    bool LoadOptionalFirmware(const std::string& logoPath, const std::string& basicPath, std::string& error);
    bool LoadOptionalFirmwareFromBuffers(std::vector<u8> logo, std::vector<u8> basic, std::string& error);
    const std::vector<u8>& LogoROM() const { return m_logoImage; }
    const std::vector<u8>& BasicROM() const { return m_basicImage; }
    std::vector<u8>& LogoROM() { return m_logoImage; }
    std::vector<u8>& BasicROM() { return m_basicImage; }
    u32 LogoCRC() const;
    u32 BasicCRC() const;
    unsigned StateVersion() const {
#if GEARSF7000_ENABLE_SR1000
        if (m_tape.HasMedia()) return 4;
#endif
        return m_logoImage.empty() && m_basicImage.empty() ? 2 : 3;
    }
#if GEARSF7000_ENABLE_SR1000
    void SetTapeValidationSource(MSXTape& tape) { m_tapeValidationSource = &tape; }
#endif
    void CopyOptionalFirmware(const SystemBusMSX& other);
    void Reset();
    void RebindMemory();
    void SaveState(std::ostream& stream) const;
    bool LoadState(std::istream& stream, unsigned version = 2);
private:
    Intel8255PinSample ReadPortA(u64 t) override { return PeekPortA(t); }
    Intel8255PinSample ReadPortB(u64 t) override { return PeekPortB(t); }
    Intel8255PinSample ReadPortC(u64 t) override { return PeekPortC(t); }
    Intel8255PinSample PeekPortA(u64) const override { return {0, 0}; }
    Intel8255PinSample PeekPortB(u64) const override { return {m_keyboard.ReadRow(m_keyboardRow), 0xFF}; }
    Intel8255PinSample PeekPortC(u64) const override { return {0xFF, 0}; }
    void DrivePortA(u8 value, u8 mask, u64) override { m_slots.SetPrimarySlots(value & mask); }
    void DrivePortB(u8, u8, u64) override {}
    void DrivePortC(u8 value, u8 mask, u64) override {
        m_keyboardRow = (value & mask) & 15;
        m_keyboardClick = (value & mask & 0x80) != 0;
#if GEARSF7000_ENABLE_SR1000
        m_tape.SetMotor((mask & 0x10) && !(value & 0x10));
        m_tape.SetOutput((value & mask & 0x20) != 0);
#endif
    }
    Memory& m_memory;
    Cartridge& m_cart;
    Audio& m_audio;
    MachineIOPorts& m_ports;
    MSXSlotMapper m_slots;
#if GEARSF7000_ENABLE_SR1000
    MSXTape m_tape;
    MSXTape* m_tapeValidationSource = nullptr;
    bool m_restoringTape = false;
#endif
    Intel8255 m_ppi;
    MSXKeyboard m_keyboard;
    u8 m_keyboardRow = 0;
    bool m_keyboardClick = false;
    std::unique_ptr<MSXLinearMemory> m_bios, m_rom, m_ram;
    std::vector<u8> m_logoImage, m_basicImage;
    std::unique_ptr<MSXLinearMemory> m_logo, m_basic;
    DebugDeviceId m_busId = 0, m_vdpId = 0, m_ayId = 0, m_ppiId = 0, m_slotsId = 0;
};
#endif
