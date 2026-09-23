/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef AIWORLD_TELEMETRYJSONCODEC_H
#define AIWORLD_TELEMETRYJSONCODEC_H

#include "AgentTelemetrySnapshot.h"

// Pure value serialization, shared by the async exporter and contract tests.
TC_GAME_API std::string SerializeAgentTelemetry(std::vector<AgentTelemetrySnapshot> const& snapshots, uint64 capturedAtMs);

#endif

