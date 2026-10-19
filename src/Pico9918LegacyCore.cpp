/* GearSF7000 - scanline backend selection for Pico replacement VDPs */
#include "Pico9918LegacyCore.h"

#define VR_EMU_TMS9918_STATIC
#include "../third_party/vrEmuTms9918/src/vrEmuTms9918.h"
#include "Pico9918LegacyCoreBridge.h"

namespace
{
class LegacyBackend final : public PicoVdpCoreBackend
{
public:
    LegacyBackend() : core(vrEmuTms9918New()) { gcPico9918LegacyPowerOn(core); }
    ~LegacyBackend() override { vrEmuTms9918Destroy(core); }
    bool IsValid() const override { return core != nullptr; }
    void Reset() override { vrEmuTms9918Reset(core); }
    void WriteControl(std::uint8_t v) override { vrEmuTms9918WriteAddr(core, v); }
    void WriteData(std::uint8_t v) override { vrEmuTms9918WriteData(core, v); }
    std::uint8_t ReadData() override { return vrEmuTms9918ReadData(core); }
    std::uint8_t ReadStatus() override { return vrEmuTms9918ReadStatus(core); }
    bool InterruptAsserted() const override { return false; }
    void RenderScanline(std::size_t line, std::uint8_t* pixels, bool row30,
        bool realY, std::uint8_t tilePal, std::uint8_t spritePal,
        std::uint8_t lineSprites, std::uint8_t sprites, std::uint8_t tileEcm,
        std::uint16_t tileOffset, std::uint8_t spriteEcm,
        std::uint16_t spriteOffset, std::uint8_t* background,
        std::uint8_t* spriteMask) override
    {
        const std::size_t height = (row30 ? 240u : 192u) *
            ((ReadRegister(0) & 0x08) ? 2u : 1u);
        if (line >= height || pixels == nullptr) return;
        const std::uint8_t saved = gcPico9918LegacyBeginScanline(core,
            static_cast<std::uint16_t>(line), row30, realY, tilePal,
            spritePal, lineSprites, sprites, tileEcm, tileOffset, spriteEcm,
            spriteOffset, background, spriteMask);
        vrEmuTms9918ScanLine(core, static_cast<std::uint16_t>(line), pixels);
        gcPico9918LegacyEndScanline(core, saved);
    }
    void EndActiveDisplay() override {}
    PicoVdpCoreMode GetMode() const override
    {
        switch (vrEmuTms9918DisplayMode(core))
        {
        case TMS_MODE_GRAPHICS_II: return PicoVdpCoreMode::GraphicsII;
        case TMS_MODE_TEXT: return PicoVdpCoreMode::Text;
        case TMS_MODE_MULTICOLOR: return PicoVdpCoreMode::Multicolor;
        default: return PicoVdpCoreMode::GraphicsI;
        }
    }
    std::uint16_t ActiveLineCount() const override { return 192; }
    std::uint16_t ReadPalette(std::size_t) const override { return 0; }
    bool IsPaletteWriteActive() const override { return false; }
    std::uint8_t ReadRegister(std::size_t i) const override
    {
        return i < TMS_NUM_REGISTERS ? vrEmuTms9918RegValue(core,
            static_cast<vrEmuTms9918Register>(i)) : 0;
    }
    void WriteRegister(std::size_t i, std::uint8_t v) override
    {
        if (i < TMS_NUM_REGISTERS) vrEmuTms9918WriteRegValue(core,
            static_cast<vrEmuTms9918Register>(i), v);
    }
    void WriteStatus(std::uint8_t v) override { gcPico9918LegacyWriteStatus(core, v); }
    void AdjustAddress(std::int16_t d) override { gcPico9918LegacyAdjustAddress(core, d); }
    std::uint8_t ReadVram(std::size_t a) const override
    {
        return a < 0x10000 ? gcPico9918LegacyReadVram(core,
            static_cast<std::uint16_t>(a)) : 0;
    }
    bool WriteVram(std::size_t a, std::uint8_t v) override
    {
        return a < 0x10000 && gcPico9918LegacyWriteVram(core,
            static_cast<std::uint16_t>(a), v);
    }
    void CancelControlLatch() override { gcPico9918LegacyCancelControlLatch(core); }
    bool ReadDebugState(PicoVdpCoreDebugState& state) const override
    {
        GcPico9918LegacyDebugState s{};
        if (!gcPico9918LegacyReadDebugState(core, &s)) return false;
        state = {s.address, s.status, s.control_latch_pending != 0,
            s.control_latch_value, s.read_ahead_buffer};
        return true;
    }
    std::size_t StateSize() const override { return gcPico9918LegacyStateSize(); }
    void SaveState(void* p) const override { gcPico9918LegacySaveState(core, p); }
    bool LoadState(const void* p, std::size_t n) override
        { return gcPico9918LegacyLoadState(core, p, n); }
private:
    VrEmuTms9918* core = nullptr;
};
}

std::unique_ptr<PicoVdpCoreBackend> CreatePico9918LegacyBackend()
{
    return std::make_unique<LegacyBackend>();
}

Pico9918LegacyCore::Pico9918LegacyCore(Backend backend, bool smsHost) : m_backend(backend)
{
#if GEARSF7000_USE_PICOSMSVDP_CORE
    if (backend == Backend::PicoSMSVdp) m_core = CreatePicoSMSVdpBackend(smsHost);
    else
#else
    (void)smsHost;
#endif
        m_core = CreatePico9918LegacyBackend();
}
Pico9918LegacyCore::~Pico9918LegacyCore() = default;
bool Pico9918LegacyCore::IsValid() const { return m_core && m_core->IsValid(); }
void Pico9918LegacyCore::Reset() { m_core->Reset(); }
void Pico9918LegacyCore::WriteControl(std::uint8_t v) { m_core->WriteControl(v); }
void Pico9918LegacyCore::WriteData(std::uint8_t v) { m_core->WriteData(v); }
std::uint8_t Pico9918LegacyCore::ReadData() { return m_core->ReadData(); }
std::uint8_t Pico9918LegacyCore::ReadStatus() { return m_core->ReadStatus(); }
bool Pico9918LegacyCore::InterruptAsserted() const { return m_core->InterruptAsserted(); }
void Pico9918LegacyCore::RenderScanline(std::size_t l, std::uint8_t* p,
    bool r, bool y, std::uint8_t tp, std::uint8_t sp, std::uint8_t ls,
    std::uint8_t s, std::uint8_t te, std::uint16_t to, std::uint8_t se,
    std::uint16_t so, std::uint8_t* bg, std::uint8_t* sm)
{ m_core->RenderScanline(l, p, r, y, tp, sp, ls, s, te, to, se, so, bg, sm); }
void Pico9918LegacyCore::EndActiveDisplay() { m_core->EndActiveDisplay(); }
Pico9918LegacyCore::Mode Pico9918LegacyCore::GetMode() const { return m_core->GetMode(); }
std::uint16_t Pico9918LegacyCore::ActiveLineCount() const { return m_core->ActiveLineCount(); }
std::uint16_t Pico9918LegacyCore::ReadPalette(std::size_t i) const { return m_core->ReadPalette(i); }
bool Pico9918LegacyCore::IsPaletteWriteActive() const { return m_core->IsPaletteWriteActive(); }
std::uint8_t Pico9918LegacyCore::ReadRegister(std::size_t i) const { return m_core->ReadRegister(i); }
void Pico9918LegacyCore::WriteRegister(std::size_t i, std::uint8_t v) { m_core->WriteRegister(i, v); }
void Pico9918LegacyCore::WriteStatus(std::uint8_t v) { m_core->WriteStatus(v); }
void Pico9918LegacyCore::AdjustAddress(std::int16_t d) { m_core->AdjustAddress(d); }
std::uint8_t Pico9918LegacyCore::ReadVram(std::size_t a) const { return m_core->ReadVram(a); }
bool Pico9918LegacyCore::WriteVram(std::size_t a, std::uint8_t v) { return m_core->WriteVram(a, v); }
void Pico9918LegacyCore::CancelControlLatch() { m_core->CancelControlLatch(); }
bool Pico9918LegacyCore::ReadDebugState(DebugState& s) const { return m_core->ReadDebugState(s); }
std::size_t Pico9918LegacyCore::StateSize() const { return m_core->StateSize(); }
void Pico9918LegacyCore::SaveState(void* p) const { m_core->SaveState(p); }
bool Pico9918LegacyCore::LoadState(const void* p, std::size_t n) { return m_core->LoadState(p, n); }
bool Pico9918LegacyCore::SupportsSmsCounters() const { return m_core->SupportsSmsCounters(); }
void Pico9918LegacyCore::SetGameGearCram(bool enabled) { m_core->SetGameGearCram(enabled); }
void Pico9918LegacyCore::ConfigureSmsRaster(bool pal) { m_core->ConfigureSmsRaster(pal); }
void Pico9918LegacyCore::LatchSmsScroll(std::uint16_t line) { m_core->LatchSmsScroll(line); }
void Pico9918LegacyCore::ClockSmsLineCounter(std::uint16_t line) { m_core->ClockSmsLineCounter(line); }
std::uint8_t Pico9918LegacyCore::ReadSmsLatchedHScroll() const { return m_core->ReadSmsLatchedHScroll(); }
std::uint8_t Pico9918LegacyCore::ReadSmsLatchedVScroll() const { return m_core->ReadSmsLatchedVScroll(); }
void Pico9918LegacyCore::SetSmsRasterPosition(std::uint16_t line, std::uint16_t column) { m_core->SetSmsRasterPosition(line, column); }
std::uint8_t Pico9918LegacyCore::ReadSmsVCounter() const { return m_core->ReadSmsVCounter(); }
void Pico9918LegacyCore::LatchSmsHCounter() { m_core->LatchSmsHCounter(); }
std::uint8_t Pico9918LegacyCore::ReadSmsHCounter() const { return m_core->ReadSmsHCounter(); }
