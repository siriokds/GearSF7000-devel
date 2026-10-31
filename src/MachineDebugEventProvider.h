/*
 * GearSF7000 - machine composition debug provider
 * Copyright (C) 2026  Saverio Russo
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef GEARSF7000_MACHINE_DEBUG_EVENT_PROVIDER_H
#define GEARSF7000_MACHINE_DEBUG_EVENT_PROVIDER_H

#include "IDebugEventProvider.h"
#include "definitions.h"

class MachineDebugEventProvider final : public IDebugEventProvider
{
public:
    explicit MachineDebugEventProvider(MachineType type) : m_type(type) {}

    DebugDeviceDefinition DescribeDebugDevice() const override
    {
        DebugDeviceDefinition device;
        device.parentKey = "event";
        if (m_type == MachineType::SG1000)
        {
            device.key = "event.sg1000";
            device.typeKey = "sega.sg1000";
            device.name = "SG-1000";
        }
        else if (m_type == MachineType::SC3000)
        {
            device.key = "event.sc3000";
            device.typeKey = "sega.sc3000";
            device.name = "SC-3000";
        }
        else if (m_type == MachineType::SordM5) {
            device.key = "event.m5"; device.typeKey = "sord.m5"; device.name = "Sord M5";
        }
        else if (m_type == MachineType::MSX)
        {
            device.key = "event.msx";
            device.typeKey = "msx.msx1";
            device.name = "MSX1";
        }
        else if (m_type == MachineType::Coleco)
        {
            device.key = "event.coleco";
            device.typeKey = "coleco.colecovision";
            device.name = "ColecoVision";
        }
        else if (m_type == MachineType::GameGear)
        {
            device.key = "event.gg";
            device.typeKey = "sega.gamegear";
            device.name = "Game Gear";
        }
        else
        {
            device.key = "event.sms";
            device.typeKey = "sega.sms";
            device.name = "Master System";
        }
        return device;
    }

private:
    MachineType m_type;
};

#endif
