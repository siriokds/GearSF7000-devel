// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026  Saverio Russo
#ifndef SN76489_DEBUG_EVENTS_H
#define SN76489_DEBUG_EVENTS_H

#include "IDebugEventProvider.h"

inline constexpr size_t kSn76489DebugSpaceCount = 2;
extern const DebugSpaceDescriptor kSn76489DebugSpaces[kSn76489DebugSpaceCount];
DebugDeviceDefinition DescribeSn76489DebugEvents();

#endif
