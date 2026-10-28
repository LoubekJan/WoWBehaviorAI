/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/NpcAgentRuntime.h"
#include <limits>

namespace
{
    NpcAgentRuntime PendingRuntime()
    {
        NpcAgentRuntime runtime;
        runtime.Guid = ObjectGuid::Create<HighGuid::Unit>(250, 31);
        runtime.Revision = 8;
        runtime.RequestId = 91;
        runtime.RequestAt = 1000;
        runtime.Pending.emplace();
        runtime.Pending->RequestId = 91;
        runtime.Pending->Agent = AgentId{42};
        runtime.Pending->SnapshotSequence = 7;
        return runtime;
    }
}

TEST_CASE("NPC reply freshness requires one current pending request and the same incarnation", "[AIWorld][NpcAgentRuntime]")
{
    auto runtime = PendingRuntime();
    uint64 requestId = 91, revision = 8, now = 13000, maxAge = 12000;
    ObjectGuid actual = runtime.Guid, echoed = runtime.Guid;
    REQUIRE(NpcAgentReplyIsFresh(runtime, requestId, actual, echoed, revision, now, maxAge));
    SECTION("missing pending slot") { runtime.Pending.reset(); }
    SECTION("zero request id") { requestId = 0; }
    SECTION("another response id") { ++requestId; }
    SECTION("another runtime slot") { ++runtime.RequestId; }
    SECTION("another pending request") { ++runtime.Pending->RequestId; }
    SECTION("empty actual incarnation") { actual.Clear(); }
    SECTION("empty runtime incarnation") { runtime.Guid.Clear(); }
    SECTION("empty echoed incarnation") { echoed.Clear(); }
    SECTION("replacement Creature") { actual = ObjectGuid::Create<HighGuid::Unit>(250, 32); }
    SECTION("older request incarnation") { echoed = ObjectGuid::Create<HighGuid::Unit>(250, 30); }
    SECTION("new dialogue input") { ++runtime.Revision; }
    SECTION("wrong echoed input revision") { ++revision; }
    SECTION("expired by one millisecond") { ++now; }
    SECTION("backwards clock") { now = runtime.RequestAt - 1; }
    CHECK_FALSE(NpcAgentReplyIsFresh(runtime, requestId, actual, echoed, revision, now, maxAge));
}

TEST_CASE("NPC input invalidation preserves admission until the old request terminates", "[AIWorld][NpcAgentRuntime]")
{
    auto runtime = PendingRuntime();
    ++runtime.Revision; // New player or NPC speech invalidates context, not its occupied HTTP slot.
    CHECK(runtime.RequestId == 91);
    REQUIRE(runtime.Pending.has_value());
    CHECK(runtime.Pending->RequestId == 91);
    CHECK_FALSE(NpcAgentReplyIsFresh(runtime, 91, runtime.Guid, runtime.Guid, 8, 2000, 12000));
    runtime.InvalidatePending(); // Lifecycle teardown releases all bindings.
    CHECK_FALSE(runtime.Pending.has_value());
    CHECK(runtime.RequestId == 0);
    CHECK(runtime.RequestAt == 0);
    CHECK_FALSE(NpcAgentReplyIsFresh(runtime, 91, runtime.Guid, runtime.Guid, runtime.Revision, 2000, 12000));
}

TEST_CASE("NPC activities require completed engine time before need effects", "[AIWorld][NpcAgentRuntime]")
{
    CHECK_FALSE(NpcAgentActivityCanComplete("EAT", 1000, 5999, true, false, true, true, true));
    CHECK(NpcAgentActivityCanComplete("EAT", 1000, 6000, true, false, true, true, true));
    CHECK_FALSE(NpcAgentActivityCanComplete("REST", 1000, 20999, true, false, true, true, true));
    CHECK(NpcAgentActivityCanComplete("REST", 1000, 21000, true, false, true, true, true));
    CHECK_FALSE(NpcAgentActivityCanComplete("WAIT", 1000, 1999, true, false, true, true, true));
    CHECK(NpcAgentActivityCanComplete("WAIT", 1000, 2000, true, false, true, true, true));
    CHECK_FALSE(NpcAgentActivityCanComplete("MOVE_TO", 1000, 21000, true, false, true, true, true));
    CHECK_FALSE(NpcAgentActivityCanComplete("NONE", 1000, 21000, true, false, true, true, true));
    CHECK_FALSE(NpcAgentActivityCanComplete("EMOTE_EAT", 1000, 21000, true, false, true, true, true));
}

TEST_CASE("Interrupted or unowned NPC animations cannot complete a meal or rest", "[AIWorld][NpcAgentRuntime]")
{
    uint64 started = 1000, now = 21000;
    bool alive = true, inCombat = false, stopped = true, posture = true, owned = true;
    SECTION("death") { alive = false; }
    SECTION("combat interruption") { inCombat = true; }
    SECTION("movement interruption") { stopped = false; }
    SECTION("posture replaced") { posture = false; }
    SECTION("goal or action replaced") { owned = false; }
    SECTION("emote without activity start") { started = 0; }
    SECTION("backwards clock") { now = started - 1; }
    SECTION("overflow cannot manufacture elapsed time") { started = std::numeric_limits<uint64>::max(); }
    CHECK_FALSE(NpcAgentActivityCanComplete("EAT", started, now, alive, inCombat, stopped, posture, owned));
    CHECK_FALSE(NpcAgentActivityCanComplete("REST", started, now, alive, inCombat, stopped, posture, owned));
}

TEST_CASE("NPC feedback preserves the engine outcome and descriptive action identity", "[AIWorld][NpcAgentRuntime]")
{
    CHECK(NpcAgentActionFeedback("ENDPOINT_MISMATCH", "MOVE_TO: Jdi směrem k místní zásobárně jídla (zásoby: 6).") ==
        "ENDPOINT_MISMATCH [MOVE_TO: Jdi směrem k místní zásobárně jídla (zásoby: 6).]");
    CHECK(NpcAgentActionFeedback("COMPLETED", "REST: Odpočívej 20 sekund.") ==
        "COMPLETED [REST: Odpočívej 20 sekund.]");
    CHECK(NpcAgentActionFeedback("READY", "") == "READY");
    CHECK(NpcAgentActionFeedback("FOOD_UNAVAILABLE", "EAT: Sněz porci ze zásob.").find("FOOD_UNAVAILABLE [EAT:") == 0);
}

TEST_CASE("NPC feedback keeps a long chosen label instead of dropping it", "[AIWorld][NpcAgentRuntime]")
{
    std::string choice = "MOVE_TO: warehouse ";
    choice.append(256, 'x');
    auto feedback = NpcAgentActionFeedback("ENDPOINT_MISMATCH", choice);
    CHECK(feedback.size() <= 128);
    CHECK(feedback.find("ENDPOINT_MISMATCH [MOVE_TO: warehouse ") == 0);
    CHECK(feedback.ends_with("...]"));
    CHECK(ValidateNpcAgentPlainText(feedback, 256, 128));
}

TEST_CASE("NPC feedback truncates multi-byte Czech and supplementary Unicode at scalar boundaries", "[AIWorld][NpcAgentRuntime]")
{
    std::string choice = "MOVE_TO: ";
    SECTION("two-byte letters")
    {
        for (unsigned i = 0; i < 100; ++i) choice += "ř";
    }
    SECTION("four-byte scalars")
    {
        for (unsigned i = 0; i < 100; ++i) choice += "\xF0\x9F\x8D\x9E";
    }
    auto feedback = NpcAgentActionFeedback("NO_COMPLETE_PATH", choice);
    CHECK(feedback.starts_with("NO_COMPLETE_PATH [MOVE_TO: "));
    CHECK(feedback.ends_with("...]"));
    CHECK(feedback.size() <= 128);
    CHECK(ValidateNpcAgentPlainText(feedback, 256, 128));
}

TEST_CASE("NPC feedback bounds oversized outcomes without hiding both identities", "[AIWorld][NpcAgentRuntime]")
{
    auto feedback = NpcAgentActionFeedback(std::string(200, 'O'), "REST: quiet rest");
    CHECK(feedback.size() <= 128);
    CHECK(feedback.starts_with(std::string(61, 'O') + "... [REST:"));
    CHECK(feedback.ends_with("quiet rest]"));
    CHECK(ValidateNpcAgentPlainText(feedback, 256, 128));
    CHECK(NpcAgentActionFeedback("COMPLETED", std::string("MOVE_TO: \xC0\xAF", 11)) == "COMPLETED [UNKNOWN_CHOICE]");
    CHECK(NpcAgentActionFeedback(std::string("\xED\xA0\x80", 3), "REST") == "UNKNOWN_RESULT [REST]");
}
