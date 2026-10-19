// SPDX-License-Identifier: GPL-3.0-or-later
#include "MSXTape.h"
#include "SegaBasicWavTape.h"
#include "SaveStateStream.h"
#include <bit>
#include <cmath>

void MSXTape::SaveState(std::ostream& stream) const
{
    StateWriter writer(stream);
    writer.U16(1);
    writer.Bool(MotorRequested);
    writer.Bool(m_output);
    writer.U64(std::bit_cast<std::uint64_t>(double(TapeSpeedPercent)));
    const auto* wave = dynamic_cast<const SegaBasicWavTape*>(Cassette);
    if (!wave) { stream.setstate(std::ios::failbit); return; }
    wave->SavePlaybackState(stream);
}

bool MSXTape::LoadState(std::istream& stream, bool apply)
{
    StateReader reader(stream);
    const auto version = reader.U16();
    const unsigned motor = reader.U8(), output = reader.U8();
    const double speed = std::bit_cast<double>(reader.U64());
    auto* wave = dynamic_cast<SegaBasicWavTape*>(Cassette);
    if (!reader.Ok() || version != 1 || motor > 1 || output > 1 ||
        !std::isfinite(speed) || speed < -20 || speed > 20 || !wave ||
        !wave->LoadPlaybackState(stream, apply)) return false;
    if (apply) {
        MotorRequested = motor; CassetteMotor->SetMotor(motor);
        m_output = output; TapeSpeedPercent = speed;
    }
    return true;
}
