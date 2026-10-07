/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/LivingAdviceState.h"
#include "Telemetry/AgentTelemetrySnapshot.h"
#include <limits>

TEST_CASE("Planning advice admission evidence separates local turns from model requests", "[AIWorld][RecoveryAdvice]")
{
    LivingAdviceState advice;
    advice.LifetimeAt = 123;
    using Event = LivingAdviceAdmissionEvent;
    advice.Admission.Record(Event::ReadyObserved, 100);
    advice.Admission.Record(Event::ReadyObserved, 200);
    advice.Admission.Record(Event::Entered, 201);
    advice.Admission.Record(Event::AcquireAttempt, 202);
    advice.Admission.Record(Event::Acquired, 203);
    advice.Admission.Record(Event::NoValidOptions, 300);
    CHECK(advice.Admission.ReadyObserved == 2);
    CHECK(advice.Admission.Entries == 1);
    CHECK(advice.Admission.AcquireAttempts == 1);
    CHECK(advice.Admission.Acquired == 1);
    CHECK(advice.Admission.LocalEmptyOptions == 1);
    CHECK(advice.Requests == 0);
    CHECK(advice.PendingId == 0);
    advice.ClearPending();
    CHECK(advice.Admission.Acquired == 1);
    CHECK(advice.Admission.LocalEmptyOptions == 1);
    advice = {};
    advice.LifetimeAt = 456;
    CHECK(advice.Admission.Acquired == 0);
    CHECK(advice.Admission.ReadyObserved == 0);
    CHECK(advice.Admission.LastEvent == Event::None);
    CHECK(advice.Admission.LastAt == 0);
}

TEST_CASE("Observer advice admission capture owns reason and handles absent and backwards clocks", "[AIWorld][Telemetry]")
{
    LivingAdviceAdmissionState source;
    auto empty = CaptureAdviceAdmissionTelemetry(source, 100);
    CHECK_FALSE(empty.LastEventAgeMs);
    CHECK_FALSE(empty.LastAcquiredAgeMs);
    source.Record(LivingAdviceAdmissionEvent::Acquired, 0);
    auto epoch = CaptureAdviceAdmissionTelemetry(source, 100);
    REQUIRE(epoch.LastAcquiredAgeMs);
    CHECK(*epoch.LastAcquiredAgeMs == 100);
    source.Record(LivingAdviceAdmissionEvent::EarlyGate, 200, "COOLDOWN");
    auto captured = CaptureAdviceAdmissionTelemetry(source, 300);
    source.Record(LivingAdviceAdmissionEvent::Entered, 400);
    CHECK(captured.LastEvent == "EARLY_GATE");
    CHECK(captured.LastReason == "COOLDOWN");
    REQUIRE(captured.LastEventAgeMs);
    CHECK(*captured.LastEventAgeMs == 100);
    CHECK_FALSE(CaptureAdviceAdmissionTelemetry(source, 399).LastEventAgeMs);
    CHECK(source.EarlyGates == 1);
}

TEST_CASE("Planning advice admission counters saturate without suppressing last event", "[AIWorld][RecoveryAdvice]")
{
    LivingAdviceAdmissionState source;
    source.Acquired = std::numeric_limits<uint32>::max();
    source.Record(LivingAdviceAdmissionEvent::Acquired, 7);
    CHECK(source.Acquired == std::numeric_limits<uint32>::max());
    CHECK(source.LastAcquiredAt == 7);
    CHECK(source.LastEvent == LivingAdviceAdmissionEvent::Acquired);
}
