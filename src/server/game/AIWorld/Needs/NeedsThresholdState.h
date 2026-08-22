/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef AIWORLD_NEEDSTHRESHOLDSTATE_H
#define AIWORLD_NEEDSTHRESHOLDSTATE_H

// Per-agent edge-trigger latch for NeedsThresholdEvent - owned by
// AgentRecord alongside NeedsState, for the same reason: it must survive
// Creature unload/reload. Without this, EvaluateThresholds() would re-emit
// an event every tick a Need stays above its enter threshold instead of
// once per crossing. Not persisted in 2.6C - resets to false on restart,
// same as NeedsState resetting to 0.0.
struct NeedsThresholdState
{
    bool HungerCriticalActive = false;
    bool DangerHighActive = false;
};

#endif // AIWORLD_NEEDSTHRESHOLDSTATE_H
