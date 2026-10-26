// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#include "SystemBusM5.h"
#include "Audio.h"
#include "Memory.h"
#include "IVideoPortDevice.h"
#include "SaveStateStream.h"
#include <limits>
#include <sstream>
#include <stdexcept>

SystemBusM5::SystemBusM5(unsigned psgWriteWaitTStates, DebugEventManager* events)
    : m_ctc(this, {"event.m5.ctc", "event.m5", "M5 Z80 CTC", events != nullptr}),
      m_events(events), m_psgWait(psgWriteWaitTStates)
{
    if (m_psgWait > 1024) throw std::invalid_argument("M5 PSG wait out of range");
    m_input.SetChangeCallback([this](unsigned target, u8 before, u8 after, u64 time) {
        Publish(m_inputId, DebugCommonSpace::CustomFirst, target, DebugEventAccess_Event,
                before, true, after, time, DebugEventSource::DeviceInternal);
    }, events != nullptr);
}

bool SystemBusM5::BindMemory(std::span<const u8> bios, std::span<const u8> cartridge,
                             std::span<u8> ram, std::span<u8> em5Ram)
{
    if (m_em5Installed != (em5Ram.size() == M5MemoryMapper::EM5RamSize) ||
        !m_mapper.Bind(bios, cartridge, ram, em5Ram)) return false;
    m_ram = ram;
    m_em5Ram = em5Ram;
    return true;
}
RamRegions SystemBusM5::GetRamRegions() const
{
    if (m_ram.empty()) return {};
    RamRegions regions{{RamRole::Internal, "internal_ram", "Internal RAM", m_ram.data(),
        M5MemoryMapper::RamSize, M5MemoryMapper::RamBase, 0, true}};
    if (m_em5Installed && !m_em5Ram.empty())
        regions.push_back({RamRole::External, "em5_ram", "EM-5 RAM", m_em5Ram.data(),
            M5MemoryMapper::EM5RamSize, M5MemoryMapper::EM5RamBase, 0, true});
    return regions;
}
void SystemBusM5::AttachCpu(CpuInterruptLines* cpu)
{
    if (cpu == m_cpu) return;
    if (m_cpu) m_cpu->SetMaskableInterruptLine(false);
    m_cpu = cpu;
    ReconcileInterrupt();
}
void SystemBusM5::ReconcileInterrupt()
{ if (m_cpu) m_cpu->SetMaskableInterruptLine(m_ctc.GetInterrupt()); }
void SystemBusM5::SetInterrupt(bool asserted, u64)
{ if (m_cpu) m_cpu->SetMaskableInterruptLine(asserted); }
void SystemBusM5::Reset()
{
    m_halfCycles = 0;
    m_command = m_printerData = 0;
    m_tapeInput = true; m_printerBusy = m_edPrefix = m_debugPause = false;
    m_input.Reset();
    m_ctc.Reset();
    // The VDP's /INT pin is active low. CLK's interrupt adapter supplies a
    // logical assertion level, while CTC channel 3 sees the physical pin.
    m_ctc.SetTrigger(3, true, CpuClock());
    ReconcileInterrupt();
}
void SystemBusM5::AdvanceCpuHalfCycles(u64 halfCycles)
{
    if (halfCycles > std::numeric_limits<u64>::max() - m_halfCycles)
        throw std::overflow_error("M5 clock overflow");
    const u64 old = CpuClock();
    m_halfCycles += halfCycles;
    const u64 now = CpuClock();
    // CPU and CTC share the same oscillator. CK2 is a square wave at CPU/8,
    // rising at 4, falling at 8, ... . Reset establishes our model's phase.
    // Deliver both edges: software may select either polarity.
    for (u64 edge = (old / 4 + 1) * 4; edge <= now; edge += 4)
        m_ctc.SetTrigger(2, (edge & 4) != 0, edge);
    m_ctc.AdvanceTo(now);
}
void SystemBusM5::SetMaskableInterruptLine(bool asserted)
{ m_ctc.SetTrigger(3, !asserted, CpuClock()); }
void SystemBusM5::ObserveOpcodeFetch(u8 value)
{
    // Peripheral M1 snooping: ED 4D only, never operand/data reads or RETN.
    // Prefix state belongs to this board and is included in its local state.
    if (m_edPrefix) {
        m_edPrefix = false;
        if (value == 0x4D) m_ctc.Reti(CpuClock());
    } else m_edPrefix = value == 0xED;
}
u8 SystemBusM5::AcknowledgeInterrupt(u64)
{
    m_edPrefix = false;
    u8 vector = 0xFF;
    m_ctc.AcknowledgeInterrupt(vector, CpuClock());
    return vector;
}
unsigned SystemBusM5::GetIOWaitStates(u64, u16 address, bool write) const
{ return write && (address & 0xF0) == 0x20 ? m_psgWait : 0; }

u8 SystemBusM5::ReadMemory(u16 address)
{
    const u8 value = m_mapper.Read(address);
    if (m_memoryDebug) m_memoryDebug->CheckBreakpoints(address, false, value, true, value, true);
    Publish(m_mapperId, DebugCommonSpace::Memory, address, DebugEventAccess_Read,
            0, false, value, CpuClock(), DebugEventSource::CpuInstruction);
    return value;
}
void SystemBusM5::WriteMemory(u16 address, u8 value)
{
    const u8 before = m_mapper.Peek(address);
    m_mapper.Write(address, value);
    if (m_memoryDebug) m_memoryDebug->CheckBreakpoints(address, true, before, true, m_mapper.Peek(address), true);
    Publish(m_mapperId, DebugCommonSpace::Memory, address, DebugEventAccess_Write,
            before, true, m_mapper.Peek(address), CpuClock(), DebugEventSource::CpuInstruction);
}
u8 SystemBusM5::ReadIO(u64, u16 address)
{
    const u8 port = address;
    u8 value = 0xFF;
    switch (port & 0xF0) {
    case 0x00: value = m_ctc.ReadRegister(port & 3, CpuClock()); break;
    case 0x10:
        if (m_video) value = port & 1 ? m_video->GetStatusFlags() : m_video->GetDataPort(CpuClock());
        break;
    case 0x30: value = (port & 7) == 7 ? m_input.ReadJoystick() : m_input.ReadRow(port & 7); break;
    case 0x50: value = u8(m_tapeInput) | (u8(m_printerBusy) << 1) | m_input.ReadResetStatus(); break;
    default: break;
    }
    if (m_memoryDebug) m_memoryDebug->CheckIOPortAccess(GetIOProbe(address), address, DebugEventAccess_Read, value);
    else Publish(m_busId, DebugCommonSpace::IoPorts, port, DebugEventAccess_Read,
            0, false, value, CpuClock(), DebugEventSource::CpuInstruction);
    return value;
}
void SystemBusM5::WriteIO(u64, u16 address, u8 value)
{
    const u8 port = address;
    switch (port & 0xF0) {
    case 0x00: m_ctc.WriteRegister(port & 3, value, CpuClock()); break;
    case 0x10:
        if (m_video) {
            if (port & 1) m_video->WriteControl(CpuClock(), value);
            else m_video->WriteData(CpuClock(), value);
        }
        break;
    case 0x20:
        if (m_audio) {
            m_audio->WriteAudioRegister(value);
            const auto& write = m_audio->GetApu()->GetLastWriteInfo();
            Publish(m_psgId, DebugCommonSpace::Registers, write.reg, DebugEventAccess_Event,
                    0, false, write.rawData, CpuClock(), DebugEventSource::CpuInstruction);
        }
        break;
    case 0x40: {
        const u8 before = m_printerData; m_printerData = value;
        Publish(m_busId, DebugCommonSpace::State, 1, DebugEventAccess_Event,
                before, true, value, CpuClock(), DebugEventSource::CpuInstruction);
        break;
    }
    case 0x50: {
        const u8 before = m_command; m_command = value & 3;
        Publish(m_busId, DebugCommonSpace::State, 0, DebugEventAccess_Event,
                before, true, m_command, CpuClock(), DebugEventSource::CpuInstruction);
        break;
    }
    default: break; // No expansion cart, FD5, SIO or ported-MSX aliases.
    }
    if (m_memoryDebug) m_memoryDebug->CheckIOPortAccess(GetIOProbe(address), address, DebugEventAccess_Write, value);
    else Publish(m_busId, DebugCommonSpace::IoPorts, port, DebugEventAccess_Write,
            0, false, value, CpuClock(), DebugEventSource::CpuInstruction);
}

void SystemBusM5::Publish(DebugDeviceId device, DebugSpaceId space, u32 target, u8 access,
                          u64 before, bool known, u64 after, u64 time, DebugEventSource source)
{
    if (m_events && device && m_events->EvaluateProbe({device, space, target}, access, before, known,
            after, true, source, DebugEventPhase::AfterCommitDeviceBoundary, m_pc ? m_pc() : 0, time)) {
        if (m_pause) m_pause();
        else m_debugPause = true;
    }
}
void SystemBusM5::OnCTCEvent(const Z80CTCEvent& event)
{
    const bool read = event.kind == Z80CTCEventKind::Read;
    const bool reg = read || event.kind == Z80CTCEventKind::Write;
    Publish(m_ctcId, reg ? DebugCommonSpace::Registers : DebugCommonSpace::State, event.channel,
        reg ? (read ? DebugEventAccess_Read : DebugEventAccess_Write) : DebugEventAccess_Event,
        0, false, reg ? event.value : static_cast<u8>(event.kind), event.timestamp,
        reg ? DebugEventSource::CpuInstruction : DebugEventSource::DeviceInternal);
}
DebugDeviceDefinition SystemBusM5::DescribeDebugDevice() const
{
    DebugDeviceDefinition d;
    d.key = "event.m5.bus"; d.parentKey = "event.m5";
    d.typeKey = "debug.system_bus"; d.name = "M5 System Bus";
    DebugSpaceDefinition io;
    io.id = DebugCommonSpace::IoPorts; io.key = "io_ports"; io.name = "I/O Ports"; io.targetMax = 255;
    io.supportedAccesses = io.afterValueAccesses = DebugEventAccess_Read | DebugEventAccess_Write;
    io.capabilities = m_events ? DebugDescriptorCapability_EventsPublished : 0;
    d.spaces.push_back(io);
    DebugSpaceDefinition state;
    state.id = DebugCommonSpace::State; state.key = "state"; state.name = "Board outputs"; state.targetMax = 1;
    state.supportedAccesses = state.beforeValueAccesses = state.afterValueAccesses = DebugEventAccess_Event;
    state.capabilities = io.capabilities;
    state.targets = {{0, "Tape / printer command", 8}, {1, "Printer data", 8}};
    d.spaces.push_back(state);
    return d;
}
DebugDeviceDefinition SystemBusM5::DescribeMapper() const
{
    auto d = m_mapper.DescribeDebugDevice();
    d.key = "event.m5.mapper"; d.parentKey = "event.m5";
    for (auto& s : d.spaces) s.capabilities = m_events ? DebugDescriptorCapability_EventsPublished : 0;
    return d;
}
DebugProbe SystemBusM5::GetIOProbe(u16 address) const
{
    const u8 port = address;
    if ((port & 0xF0) == 0x10 && m_vdpId)
        return {m_vdpId, DebugCommonSpace::IoPorts, u32(0x10 | (port & 1))};
    return {m_busId, DebugCommonSpace::IoPorts, port};
}
bool SystemBusM5::BindDebugEvents(const DebugDeviceRegistry& registry)
{
    const auto bus = registry.FindDevice("event.m5.bus");
    const auto mapper = registry.FindDevice("event.m5.mapper");
    const auto ctc = registry.FindDevice("event.m5.ctc");
    const auto input = registry.FindDevice("event.m5.input");
    const auto psg = registry.FindDevice("event.m5.psg");
    if (!m_events || &m_events->GetRegistry() != &registry || !bus || !mapper || !ctc || !input || !psg) return false;
    const auto vdp = registry.FindDevice("event.m5.vdp");
    m_vdpId = vdp ? vdp->id : 0;
    m_busId = bus->id; m_mapperId = mapper->id; m_ctcId = ctc->id; m_inputId = input->id; m_psgId = psg->id;
    return true;
}
void SystemBusM5::SaveState(std::ostream& stream) const
{
    StateWriter w(stream);
    w.U16(2); w.U16(m_psgWait); w.Bool(m_em5Installed); w.U64(m_halfCycles);
    w.U8(m_command); w.U8(m_printerData); w.Bool(m_tapeInput); w.Bool(m_printerBusy); w.Bool(m_edPrefix);
    m_ctc.SaveState(stream); m_input.SaveState(stream);
}
bool SystemBusM5::LoadState(std::istream& stream)
{
    StateReader r(stream);
    const u16 version = r.U16();
    if ((version != 1 && version != 2) || r.U16() != m_psgWait) return false;
    const u8 installed = version == 2 ? r.U8() : 0;
    if (installed > 1 || bool(installed) != m_em5Installed) return false;
    const u64 halfCycles = r.U64();
    const u8 command = r.U8(), printer = r.U8(), tape = r.U8(), busy = r.U8(), prefix = r.U8();
    Z80CTC ctc;
    M5Input input;
    if (!r.Ok() || command > 3 || tape > 1 || busy > 1 || prefix > 1 ||
        !ctc.LoadState(stream) || !input.LoadState(stream)) return false;
    const auto snapshot = ctc.GetSnapshot();
    if (snapshot.timestamp != halfCycles / 2 || !snapshot.interruptEnableIn ||
        snapshot.channels[0].triggerHigh || snapshot.channels[1].triggerHigh ||
        snapshot.channels[2].triggerHigh != bool((halfCycles / 2) & 4)) return false;
    // Re-encode validated fixed-size state to retain live pin/callback bindings.
    std::stringstream ctcState, inputState;
    ctc.SaveState(ctcState); input.SaveState(inputState);
    if (!m_ctc.LoadState(ctcState) || !m_input.LoadState(inputState)) return false;
    m_halfCycles = halfCycles; m_command = command; m_printerData = printer;
    m_tapeInput = tape; m_printerBusy = busy; m_edPrefix = prefix; m_debugPause = false;
    return true;
}
