/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/NpcAgentRuntime.h"
#include <limits>

namespace
{
    NpcAgentPendingTurn PlayerTurn(uint64 id, uint32 player = 1, uint64 at = 1000, std::string text = "Kde je sklad?")
    { return {id, ObjectGuid::Create<HighGuid::Player>(player), "Hrac" + std::to_string(player), std::move(text), at}; }

    NpcAgentPendingTurn NpcTurn(uint64 id, uint64 at = 1000)
    { return {id, ObjectGuid::Create<HighGuid::Unit>(250, 2), "Běla", "Jak se daří?", at}; }

    NpcAgentSpeech Spoken(uint64 id, uint64 at, std::string text)
    {
        NpcAgentSpeech line;
        line.ConversationId = line.UtteranceId = id; line.Speaker = AgentId{901035};
        line.SpeakerName = "Matěj"; line.Kind = "NPC"; line.Text = std::move(text); line.AtMs = at; line.Hop = 1;
        return line;
    }

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
    runtime.AppearanceApplied = true;
    runtime.InvalidatePending(); // Lifecycle teardown releases all bindings.
    CHECK_FALSE(runtime.AppearanceApplied); // Same-GUID respawns must restore their authored outfit.
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

TEST_CASE("Pinned player turns survive ambient traffic and use only their private partner history", "[AIWorld][NpcAgentSpeech]")
{
    NpcAgentRuntime runtime;
    auto first = PlayerTurn(100);
    REQUIRE(runtime.Speech.PinPlayer(first));
    runtime.Revision = 7;
    for (uint64 id = 200; id < 220; ++id)
    {
        CHECK_FALSE(runtime.Speech.PinNpc(NpcTurn(id), 1000));
        // Even older shared transcript rows cannot displace the independent pin.
        REQUIRE(runtime.Dialogue.Remember(Spoken(id, 1000, "Village conversation")));
    }
    REQUIRE(runtime.Speech.PlayerTurn.has_value());
    CHECK(runtime.Speech.PlayerTurn->Id == first.Id);
    CHECK(runtime.Speech.PlayerTurn->Partner == first.Partner);
    CHECK(runtime.Speech.PlayerTurn->Text == first.Text);
    CHECK(runtime.Revision == 7);
    auto const* history = runtime.Speech.PrivateHistory(first.Partner);
    REQUIRE(history);
    REQUIRE(history->Lines().size() == 1);
    CHECK(history->Lines().front().Text == first.Text);
    CHECK(runtime.Speech.PrivateHistory(ObjectGuid::Create<HighGuid::Player>(2)) == nullptr);
}

TEST_CASE("Only successful sending to the exact pinned player turn acknowledges it", "[AIWorld][NpcAgentSpeech]")
{
    NpcAgentSpeechState state;
    auto first = PlayerTurn(100);
    REQUIRE(state.PinPlayer(first));
    REQUIRE(state.CanSend("PLAYER_REPLY", 100, first.Partner, "Sklad je u cesty.", 2000));
    // A provider failure, silence or rejected/disconnected delivery has no commit.
    CHECK_FALSE(state.CanSend("PLAYER_REPLY", 100, first.Partner, "", 2000));
    CHECK_FALSE(state.CanSend("SILENT", 100, first.Partner, "Sklad je u cesty.", 2000));
    CHECK_FALSE(state.CanSend("PLAYER_REPLY", 99, first.Partner, "Sklad je u cesty.", 2000));
    CHECK_FALSE(state.CanSend("PLAYER_REPLY", 100, ObjectGuid::Create<HighGuid::Player>(2), "Sklad je u cesty.", 2000));
    REQUIRE(state.PlayerTurn.has_value());
    CHECK(state.PlayerTurn->Id == 100);
    REQUIRE(state.CommitSent("PLAYER_REPLY", 100, first.Partner, Spoken(300, 2000, "Sklad je u cesty."), 2000));
    CHECK_FALSE(state.PlayerTurn.has_value());
    CHECK_FALSE(state.CanSend("PLAYER_REPLY", 100, first.Partner, "Sklad je u cesty.", 2001));
    REQUIRE(state.PrivateHistory(first.Partner));
    CHECK(state.PrivateHistory(first.Partner)->Lines().size() == 2);
    CHECK(state.PrivateHistory(first.Partner)->Lines().back().Kind == "SELF");
}

TEST_CASE("A replaced player question cannot consume or redirect the newer turn", "[AIWorld][NpcAgentSpeech]")
{
    NpcAgentSpeechState state;
    auto old = PlayerTurn(100);
    auto next = PlayerTurn(101, 2, 2000, "Jak se jmenuješ?");
    REQUIRE(state.PinPlayer(old));
    REQUIRE(state.PinPlayer(next));
    CHECK_FALSE(state.CommitSent("PLAYER_REPLY", old.Id, old.Partner, Spoken(300, 2100, "Old answer"), 2100));
    CHECK_FALSE(state.CanSend("PLAYER_REPLY", next.Id, old.Partner, "Wrong recipient", 2100));
    REQUIRE(state.PlayerTurn.has_value());
    CHECK(state.PlayerTurn->Id == next.Id);
    CHECK(state.PlayerTurn->Partner == next.Partner);
    REQUIRE(state.PrivateHistory(old.Partner));
    REQUIRE(state.PrivateHistory(next.Partner));
    CHECK(state.PrivateHistory(old.Partner)->Lines().front().Text == old.Text);
    CHECK(state.PrivateHistory(next.Partner)->Lines().front().Text == next.Text);
}

TEST_CASE("New repeated player questions bypass ambient cooldown and text deduplication", "[AIWorld][NpcAgentSpeech]")
{
    NpcAgentSpeechState state;
    REQUIRE(state.CommitSent("INITIATE", 0, ObjectGuid{}, Spoken(1, 1000, "Dobrý den."), 1000));
    CHECK_FALSE(state.CanAmbient(1001));
    auto first = PlayerTurn(100, 1, 1001, "Pozdravíš mě?");
    REQUIRE(state.PinPlayer(first));
    REQUIRE(state.CommitSent("PLAYER_REPLY", 100, first.Partner, Spoken(2, 1001, "Dobrý den."), 1001));
    auto repeated = PlayerTurn(101, 1, 1002, first.Text);
    REQUIRE(state.PinPlayer(repeated));
    REQUIRE(state.CommitSent("PLAYER_REPLY", 101, repeated.Partner, Spoken(3, 1002, "Dobrý den."), 1002));
    CHECK_FALSE(state.CanAmbient(61001));
    CHECK(state.CanAmbient(61002));
    CHECK_FALSE(state.CanSend("PLAYER_REPLY", 101, repeated.Partner, "Dobrý den.", 61002));
    CHECK_FALSE(state.CanSend("INITIATE", 0, repeated.Partner, "Unsolicited whisper", 61002));
}

TEST_CASE("Autonomous speech requires a fresh scene and consumes silent initiation opportunities", "[AIWorld][NpcAgentSpeech]")
{
    NpcAgentSpeechState state;
    CHECK_FALSE(state.CanInitiate(11, false, 1000));
    REQUIRE(state.CanInitiate(11, true, 1000));
    state.SubmittedInitiation(11); // Even a silent answer consumes this scene.
    CHECK_FALSE(state.CanInitiate(11, true, 9000));
    CHECK_FALSE(state.CanInitiate(11, true, 61000));
    CHECK(state.CanInitiate(12, true, 61000));
    auto player = PlayerTurn(100, 1, 61000);
    REQUIRE(state.PinPlayer(player));
    CHECK_FALSE(state.CanInitiate(12, true, 61000));
}

TEST_CASE("Ambient cooldown and normalized-repeat windows apply only after successful sends", "[AIWorld][NpcAgentSpeech]")
{
    NpcAgentSpeechState state;
    REQUIRE(state.CanSend("INITIATE", 0, ObjectGuid{}, "ŘEČ u SKLADU!", 1000));
    // A failed engine send does not consume the available budget.
    CHECK(state.CanSend("INITIATE", 0, ObjectGuid{}, "řeč u skladu", 1001));
    REQUIRE(state.CommitSent("INITIATE", 0, ObjectGuid{}, Spoken(1, 1001, "ŘEČ u SKLADU!"), 1001));
    CHECK_FALSE(state.CanAmbient(61000, "Another sentence"));
    CHECK_FALSE(state.CanAmbient(61001, "  řeč   u skladu...  "));
    REQUIRE(state.CanAmbient(61001, "Another sentence"));
    REQUIRE(state.CommitSent("INITIATE", 0, ObjectGuid{}, Spoken(2, 61001, "Another sentence"), 61001));
    CHECK_FALSE(state.CanAmbient(121000, "řeč u skladu"));
    CHECK(state.CanAmbient(121001, "řeč u skladu"));
    CHECK(NormalizeNpcAgentSpeech("  ŘEČ u SKLADU!  ") == NormalizeNpcAgentSpeech("řeč   u skladu..."));
    CHECK(NormalizeNpcAgentSpeech("ŠŤASTNÝ ŽIVOT, ÚRODA.") == NormalizeNpcAgentSpeech("šťastný život úroda"));
}

TEST_CASE("Only a fresh NPC turn with available ambient budget permits a social reply", "[AIWorld][NpcAgentSpeech]")
{
    NpcAgentSpeechState state;
    auto turn = NpcTurn(100);
    REQUIRE(state.PinNpc(turn, 1000));
    CHECK_FALSE(state.CanSend("NPC_REPLY", 99, turn.Partner, "Daří se dobře.", 1001));
    REQUIRE(state.CanSend("NPC_REPLY", 100, turn.Partner, "Daří se dobře.", 15999));
    CHECK_FALSE(state.CanSend("NPC_REPLY", 100, turn.Partner, "Daří se dobře.", 16000));
    REQUIRE(state.Expire(16000));
    CHECK_FALSE(state.NpcTurn.has_value());
    REQUIRE(state.PinPlayer(PlayerTurn(200, 1, 16000)));
    CHECK_FALSE(state.PinNpc(NpcTurn(201, 16000), 16000));
    REQUIRE(state.CommitSent("PLAYER_REPLY", 200, ObjectGuid::Create<HighGuid::Player>(1), Spoken(300, 16000, "Ano."), 16000));
    CHECK_FALSE(state.PinNpc(NpcTurn(202, 16001), 16001));
    REQUIRE(state.PinNpc(NpcTurn(203, 76000), 76000));
    REQUIRE(state.CommitSent("NPC_REPLY", 203, turn.Partner, Spoken(301, 76000, "Daří se dobře."), 76000));
    CHECK_FALSE(state.NpcTurn.has_value());
}

TEST_CASE("Pinned player TTL disconnect and NPC lifecycle release turns explicitly", "[AIWorld][NpcAgentSpeech]")
{
    NpcAgentRuntime runtime;
    auto turn = PlayerTurn(100);
    REQUIRE(runtime.Speech.PinPlayer(turn));
    CHECK_FALSE(runtime.Speech.Expire(60999));
    REQUIRE(runtime.Speech.PlayerTurn.has_value());
    CHECK(runtime.Speech.Expire(61000));
    CHECK_FALSE(runtime.Speech.PlayerTurn.has_value());
    REQUIRE(runtime.Speech.PinPlayer(PlayerTurn(101, 1, 61000)));
    CHECK(runtime.Speech.Expire(61001, false));
    CHECK_FALSE(runtime.Speech.PlayerTurn.has_value());
    REQUIRE(runtime.Speech.PinPlayer(PlayerTurn(102, 1, 61002)));
    runtime.InvalidatePending();
    CHECK_FALSE(runtime.Speech.PlayerTurn.has_value());
    CHECK_FALSE(runtime.Speech.NpcTurn.has_value());
    CHECK_FALSE(runtime.Speech.CanAmbient(61000)); // Retained monotonic clock watermark.
}

TEST_CASE("Private dialogue history is bounded per interlocutor without cross-player rows", "[AIWorld][NpcAgentSpeech]")
{
    NpcAgentSpeechState state;
    for (uint32 partner = 1; partner <= 5; ++partner)
        REQUIRE(state.PinPlayer(PlayerTurn(100 + partner, partner, 1000 + partner, "Question " + std::to_string(partner))));
    CHECK(state.PrivateHistory(ObjectGuid::Create<HighGuid::Player>(1)) == nullptr);
    for (uint32 partner = 2; partner <= 5; ++partner)
    {
        auto const* history = state.PrivateHistory(ObjectGuid::Create<HighGuid::Player>(partner));
        REQUIRE(history);
        REQUIRE(history->Lines().size() == 1);
        CHECK(history->Lines().front().Text == "Question " + std::to_string(partner));
    }
    for (uint64 id = 200; id < 220; ++id) REQUIRE(state.PinPlayer(PlayerTurn(id, 5, 2000 + id)));
    auto const* history = state.PrivateHistory(ObjectGuid::Create<HighGuid::Player>(5));
    REQUIRE(history);
    CHECK(history->Lines().size() == 12);
    CHECK(history->Lines().back().UtteranceId == 219);
}

TEST_CASE("A private answer cannot become an autonomous replay but a new player turn can repeat it", "[AIWorld][NpcAgentSpeech]")
{
    NpcAgentSpeechState state;
    auto first = PlayerTurn(100);
    REQUIRE(state.PinPlayer(first));
    REQUIRE(state.CommitSent("PLAYER_REPLY", first.Id, first.Partner, Spoken(300, 1000, "Sklad je u cesty."), 1000));
    CHECK_FALSE(state.CanAmbient(61000, "SKLAD je u cesty!"));
    auto repeated = PlayerTurn(101, 1, 61001, first.Text);
    REQUIRE(state.PinPlayer(repeated));
    REQUIRE(state.CommitSent("PLAYER_REPLY", repeated.Id, repeated.Partner, Spoken(301, 61001, "Sklad je u cesty."), 61001));
    CHECK_FALSE(state.CanAmbient(121001, "Sklad je u cesty."));
    CHECK(state.CanAmbient(181001, "Sklad je u cesty."));
}
