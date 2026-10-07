/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGADVICEADMISSION_H
#define AIWORLD_LIVINGADVICEADMISSION_H
#include "Define.h"
#include <limits>

enum class LivingAdviceAdmissionEvent : uint8
{ None, ReadyObserved, Entered, AcquireAttempt, Acquired, EarlyGate, NoValidOptions };

inline char const* ToString(LivingAdviceAdmissionEvent event)
{
    switch (event)
    {
        case LivingAdviceAdmissionEvent::ReadyObserved: return "READY_OBSERVED";
        case LivingAdviceAdmissionEvent::Entered: return "ENTERED";
        case LivingAdviceAdmissionEvent::AcquireAttempt: return "ACQUIRE_ATTEMPT";
        case LivingAdviceAdmissionEvent::Acquired: return "ACQUIRED";
        case LivingAdviceAdmissionEvent::EarlyGate: return "EARLY_GATE";
        case LivingAdviceAdmissionEvent::NoValidOptions: return "NO_VALID_OPTIONS";
        default: return "NONE";
    }
}

// Fixed-size per-incarnation evidence, independent of search/pending resets.
// ReadyObserved counts true Ready calls, not distinct offers or model requests.
struct LivingAdviceAdmissionState
{
    uint32 ReadyObserved = 0, Entries = 0, AcquireAttempts = 0, Acquired = 0,
        EarlyGates = 0, LocalEmptyOptions = 0;
    LivingAdviceAdmissionEvent LastEvent = LivingAdviceAdmissionEvent::None;
    char const* LastReason = "NONE";
    uint64 LastAt = 0, LastAcquiredAt = 0;

    // reason must be a static string literal: capture owns a copy before async export.
    void Record(LivingAdviceAdmissionEvent event, uint64 nowMs, char const* reason = nullptr)
    {
        uint32* counter = nullptr;
        switch (event)
        {
            case LivingAdviceAdmissionEvent::ReadyObserved: counter = &ReadyObserved; break;
            case LivingAdviceAdmissionEvent::Entered: counter = &Entries; break;
            case LivingAdviceAdmissionEvent::AcquireAttempt: counter = &AcquireAttempts; break;
            case LivingAdviceAdmissionEvent::Acquired: counter = &Acquired; LastAcquiredAt = nowMs; break;
            case LivingAdviceAdmissionEvent::EarlyGate: counter = &EarlyGates; break;
            case LivingAdviceAdmissionEvent::NoValidOptions: counter = &LocalEmptyOptions; break;
            default: return;
        }
        if (*counter < std::numeric_limits<uint32>::max()) ++*counter;
        LastEvent = event; LastReason = reason ? reason : ToString(event); LastAt = nowMs;
    }
};
#endif
