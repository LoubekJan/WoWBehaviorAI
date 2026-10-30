/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/NpcAgentRuntime.h"

namespace
{
    ObjectGuid MemoryPlayer(uint32 id) { return ObjectGuid::Create<HighGuid::Player>(id); }
    bool HasMemory(std::vector<NpcAgentMemory> const& entries, std::string_view text)
    { return std::any_of(entries.begin(), entries.end(), [&](auto const& entry) { return entry.Text == text; }); }
}

TEST_CASE("NPC episodic recall isolates exact players even with identical display names", "[AIWorld][NpcAgentMemory]")
{
    NpcAgentMemoryStore memory;
    REQUIRE(memory.RememberPlayer(1, MemoryPlayer(1), "Calvin", "Moje heslo je modrá.", 100));
    REQUIRE(memory.RememberPlayer(2, MemoryPlayer(2), "Calvin", "Moje heslo je zelená.", 101));
    REQUIRE(memory.RememberNpc(3, "Běla", "Dnes je u vody klid.", 102));
    auto first = memory.Retrieve(200, "heslo", MemoryPlayer(1));
    CHECK(HasMemory(first, "Moje heslo je modrá."));
    CHECK_FALSE(HasMemory(first, "Moje heslo je zelená."));
    auto second = memory.Retrieve(200, "heslo", MemoryPlayer(2));
    CHECK_FALSE(HasMemory(second, "Moje heslo je modrá."));
    CHECK(HasMemory(second, "Moje heslo je zelená."));
    for (auto const& entry : memory.Retrieve(200, "heslo")) CHECK(entry.Kind != "PLAYER_DIALOGUE");
    for (auto const& entry : memory.Retrieve(200, "heslo", ObjectGuid::Create<HighGuid::Unit>(250, 7)))
        CHECK(entry.Kind != "PLAYER_DIALOGUE");
    CHECK(HasMemory(first, "Dnes je u vody klid."));
    NpcAgentMemoryStore anotherNpc;
    CHECK(anotherNpc.Retrieve(200, "heslo", MemoryPlayer(1)).empty());
}

TEST_CASE("NPC memories distinguish unanswered quotes from actually delivered replies", "[AIWorld][NpcAgentMemory]")
{
    NpcAgentMemoryStore memory;
    REQUIRE(memory.RememberPlayer(11, MemoryPlayer(1), "Calvin", "Pamatuj si mou modrou stuhu.", 100));
    CHECK(memory.Retrieve(110, "stuha", MemoryPlayer(1), 11).empty());
    auto unanswered = memory.Retrieve(110, "stuha", MemoryPlayer(1));
    REQUIRE(unanswered.size() == 1);
    CHECK(unanswered[0].Response.empty());
    CHECK(unanswered[0].AgeMs == 10);
    CHECK_FALSE(memory.RememberPlayerReply(11, MemoryPlayer(2), "Rozumím.", 120));
    CHECK_FALSE(memory.RememberPlayerReply(12, MemoryPlayer(1), "Rozumím.", 120));
    REQUIRE(memory.RememberPlayerReply(11, MemoryPlayer(1), "Modrou stuhu si zapamatuji.", 120));
    CHECK_FALSE(memory.RememberPlayerReply(11, MemoryPlayer(1), "Jiná neodeslaná odpověď.", 121));
    auto answered = memory.Retrieve(150, "stuha", MemoryPlayer(1));
    REQUIRE(answered.size() == 1);
    CHECK(answered[0].Id == unanswered[0].Id);
    CHECK(answered[0].Response == "Modrou stuhu si zapamatuji.");
    CHECK(answered[0].AgeMs == 30);
}

TEST_CASE("NPC keyword recall recovers an older relevant exchange beyond short dialogue", "[AIWorld][NpcAgentMemory]")
{
    NpcAgentMemoryStore memory;
    NpcAgentDialogueHistory shortHistory;
    REQUIRE(memory.RememberPlayer(1, MemoryPlayer(1), "Calvin", "Moje oblíbená barva je modrá.", 100));
    REQUIRE(memory.RememberPlayerReply(1, MemoryPlayer(1), "Modrou mám také rád.", 101));
    NpcAgentSpeech initial;
    initial.ConversationId = initial.UtteranceId = 1; initial.Kind = "PLAYER";
    initial.SpeakerName = "Calvin"; initial.Text = "Moje oblíbená barva je modrá."; initial.AtMs = 100;
    REQUIRE(shortHistory.Remember(initial));
    for (uint64 i = 2; i <= 26; ++i)
    {
        std::string line = "Dnes probíráme nářadí číslo " + std::to_string(i) + ".";
        REQUIRE(memory.RememberPlayer(i, MemoryPlayer(1), "Calvin", line, 100 + i));
        NpcAgentSpeech speech;
        speech.ConversationId = speech.UtteranceId = i; speech.Kind = "PLAYER";
        speech.SpeakerName = "Calvin"; speech.Text = line; speech.AtMs = 100 + i;
        REQUIRE(shortHistory.Remember(speech));
    }
    CHECK(shortHistory.Lines().size() == 12);
    CHECK(std::none_of(shortHistory.Lines().begin(), shortHistory.Lines().end(), [&](auto const& line)
        { return line.Text == initial.Text; }));
    CHECK(memory.Size() == 26);
    auto recalled = memory.Retrieve(200, "Jakou BARVU mám rád?", MemoryPlayer(1));
    REQUIRE(recalled.size() == NpcAgentMemoryStore::MaxRecalled);
    REQUIRE(HasMemory(recalled, "Moje oblíbená barva je modrá."));
    CHECK(recalled.front().Text == "Moje oblíbená barva je modrá.");
    CHECK(recalled.front().Response == "Modrou mám také rád.");
    CHECK(recalled.front().Relevance >= 1000);
    for (std::size_t i = 1; i < recalled.size(); ++i) CHECK(recalled[i - 1].AgeMs >= recalled[i].AgeMs);
}

TEST_CASE("NPC memory evicts within bounded private public and partner budgets", "[AIWorld][NpcAgentMemory]")
{
    SECTION("one player cannot exceed its quota or exhaust public memory")
    {
        NpcAgentMemoryStore memory;
        for (uint64 i = 1; i <= 40; ++i)
            REQUIRE(memory.RememberPlayer(i, MemoryPlayer(1), "Calvin", "Soukromá poznámka " + std::to_string(i), i));
        CHECK(memory.Size() == NpcAgentMemoryStore::MaxRecordsPerPlayer);
        CHECK_FALSE(HasMemory(memory.Retrieve(100, "poznámka", MemoryPlayer(1)), "Soukromá poznámka 1"));
        for (uint64 i = 41; i <= 140; ++i)
            REQUIRE(memory.RememberNpc(i, "Běla", "Veřejná poznámka " + std::to_string(i), i));
        CHECK(memory.Size() == NpcAgentMemoryStore::MaxRecordsPerPlayer + NpcAgentMemoryStore::MaxPublicRecords);
        CHECK_FALSE(HasMemory(memory.Retrieve(200, "poznámka"), "Veřejná poznámka 41"));
    }
    SECTION("a ninth partner evicts only the oldest partner")
    {
        NpcAgentMemoryStore memory;
        for (uint32 i = 1; i <= 9; ++i)
            REQUIRE(memory.RememberPlayer(i, MemoryPlayer(i), "Calvin", "Poznámka hráče " + std::to_string(i), i));
        CHECK(memory.Size() == NpcAgentMemoryStore::MaxPlayers);
        CHECK(memory.Retrieve(20, "poznámka", MemoryPlayer(1)).empty());
        CHECK_FALSE(memory.Retrieve(20, "poznámka", MemoryPlayer(2)).empty());
    }
    SECTION("global private capacity remains bounded across all partners")
    {
        NpcAgentMemoryStore memory;
        for (uint64 i = 1; i <= 160; ++i)
            REQUIRE(memory.RememberPlayer(i, MemoryPlayer(uint32(i % 8 + 1)), "Calvin", "Poznámka " + std::to_string(i), i));
        CHECK(memory.Size() == NpcAgentMemoryStore::MaxPrivateRecords);
    }
}

TEST_CASE("NPC memory validates UTF8 and bounds recalled bytes without truncating quotations", "[AIWorld][NpcAgentMemory]")
{
    NpcAgentMemoryStore memory;
    CHECK_FALSE(memory.RememberPlayer(0, MemoryPlayer(1), "Calvin", "Ahoj.", 1));
    CHECK_FALSE(memory.RememberPlayer(1, {}, "Calvin", "Ahoj.", 1));
    CHECK_FALSE(memory.RememberPlayer(1, MemoryPlayer(1), "Calvin", "|cffffffff falešná zpráva", 1));
    CHECK_FALSE(memory.RememberPlayer(1, MemoryPlayer(1), "Calvin", "Dva\nřádky", 1));
    CHECK_FALSE(memory.RememberPlayer(1, MemoryPlayer(1), std::string(33, 'a'), "Ahoj.", 1));
    CHECK_FALSE(memory.RememberPlayer(1, MemoryPlayer(1), "Calvin", std::string("\xC0\xAF", 2), 1));
    std::string longText;
    for (unsigned i = 0; i < 256; ++i) longText += "界";
    for (uint64 i = 1; i <= 10; ++i)
    {
        REQUIRE(memory.RememberPlayer(i, MemoryPlayer(1), "Matěj", longText, i * 2));
        REQUIRE(memory.RememberPlayerReply(i, MemoryPlayer(1), longText, i * 2 + 1));
    }
    auto entries = memory.Retrieve(100, "Matěj", MemoryPlayer(1));
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].Text == longText);
    CHECK(entries[0].Response == longText);
    std::size_t bytes = 0;
    for (auto const& entry : entries) bytes += entry.Speaker.size() + entry.Text.size() + entry.Response.size();
    CHECK(bytes <= NpcAgentMemoryStore::MaxRecallBytes);
}

TEST_CASE("NPC memory rejects duplicate sources clock regression and future recall", "[AIWorld][NpcAgentMemory]")
{
    NpcAgentMemoryStore memory;
    REQUIRE(memory.RememberNpc(1, "Běla", "Znám cestu k vodě.", 100));
    CHECK_FALSE(memory.RememberNpc(1, "Běla", "Jiná vymyšlená věta.", 101));
    CHECK_FALSE(memory.RememberNpc(2, "Běla", "Příliš stará věta.", 99));
    CHECK(memory.Retrieve(99, "voda").empty());
    REQUIRE(memory.RememberAction("Matěj", "MOVE_FAILED [MOVE_TO: cesta k vodě]", 110));
    REQUIRE(memory.RememberAction("Matěj", "MOVE_FAILED [MOVE_TO: cesta k vodě]", 120));
    CHECK(memory.Size() == 2);
    auto entries = memory.Retrieve(130, "voda");
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].Kind == "NPC_DIALOGUE");
    CHECK(entries[0].AgeMs == 30);
    CHECK(entries[1].Kind == "ACTION_RESULT");
    CHECK(entries[1].AgeMs == 10);
    CHECK(entries[0].Id != entries[1].Id);
    REQUIRE(memory.RememberPlayer(7, MemoryPlayer(1), "Calvin", "Tohle jsem opravdu řekl.", 130));
    CHECK_FALSE(memory.RememberPlayer(7, MemoryPlayer(1), "Calvin", "Nahrazený výrok.", 140));
    CHECK_FALSE(memory.RememberPlayerReply(7, MemoryPlayer(1), "Odpověď z minulosti.", 129));
    CHECK(memory.Retrieve(150, "opravdu", MemoryPlayer(1)).back().Response.empty());
}

TEST_CASE("NPC incarnation rebind keeps only stable NPC session memories", "[AIWorld][NpcAgentMemory]")
{
    NpcAgentRuntime runtime;
    auto previous = ObjectGuid::Create<HighGuid::Unit>(250, 1);
    auto replacement = ObjectGuid::Create<HighGuid::Unit>(250, 2);
    runtime.Guid = previous; runtime.RequestId = 91; runtime.Revision = 8; runtime.ActionAt = 100;
    runtime.Pending.emplace(); runtime.Pending->RequestId = 91;
    runtime.Active = "MOVE_TO"; runtime.Actions.emplace(1, NpcAgentActionBinding{});
    runtime.Recipients.emplace(1, NpcAgentRecipientBinding{MemoryPlayer(1), true});
    REQUIRE(runtime.Speech.PinPlayer({1, MemoryPlayer(1), "Calvin", "Moje barva je modrá.", 100}));
    REQUIRE(runtime.Memory.RememberPlayer(1, MemoryPlayer(1), "Calvin", "Moje barva je modrá.", 100));
    runtime.MemoryRecalled = 1;
    runtime.Rebind(replacement);
    CHECK(runtime.Guid == replacement);
    CHECK(runtime.RequestId == 0);
    CHECK(runtime.ActionAt == 0);
    CHECK(runtime.Active == "NONE");
    CHECK_FALSE(runtime.Pending.has_value());
    CHECK_FALSE(runtime.Speech.PlayerTurn.has_value());
    CHECK(runtime.Actions.empty());
    CHECK(runtime.Recipients.empty());
    CHECK(runtime.MemoryRecalled == 0);
    CHECK(runtime.Memory.Size() == 1);
    CHECK(HasMemory(runtime.Memory.Retrieve(200, "barva", MemoryPlayer(1)), "Moje barva je modrá."));
    CHECK(runtime.Memory.Retrieve(200, "barva").empty());
    runtime.InvalidatePending();
    CHECK(runtime.Memory.Size() == 1);
}

TEST_CASE("NPC private recall normalizes a renamed same GUID and budgets the current name", "[AIWorld][NpcAgentMemory]")
{
    NpcAgentMemoryStore memory;
    std::string text(256, 'a'), response(250, 'b');
    for (uint64 i = 1; i <= 6; ++i)
    {
        REQUIRE(memory.RememberPlayer(i, MemoryPlayer(1), "A", text, i * 2));
        REQUIRE(memory.RememberPlayerReply(i, MemoryPlayer(1), response, i * 2 + 1));
    }
    auto old = memory.Retrieve(100, "", MemoryPlayer(1));
    REQUIRE(old.size() == 6);
    std::string renamed(32, 'Z');
    auto current = memory.Retrieve(100, "", MemoryPlayer(1), 0, renamed);
    REQUIRE(current.size() == 5);
    std::size_t bytes = 0;
    for (auto const& entry : current)
    {
        CHECK(entry.Speaker == renamed);
        CHECK(entry.Text == text);
        CHECK(entry.Response == response);
        bytes += entry.Speaker.size() + entry.Text.size() + entry.Response.size();
    }
    CHECK(bytes <= NpcAgentMemoryStore::MaxRecallBytes);
    CHECK(memory.Retrieve(100, "", MemoryPlayer(1))[0].Speaker == "A");
    CHECK(memory.Retrieve(100, "", MemoryPlayer(2), 0, renamed).empty());
}

TEST_CASE("NPC memory capacity evicts the oldest observation rather than a recently refreshed outcome", "[AIWorld][NpcAgentMemory]")
{
    NpcAgentMemoryStore memory;
    REQUIRE(memory.RememberAction("Matěj", "MOVE_FAILED [MOVE_TO: voda]", 1));
    for (uint64 i = 2; i <= 32; ++i)
        REQUIRE(memory.RememberNpc(i, "Běla", i == 2 ? "Zapamatovaný jantar." : "Veřejná poznámka " + std::to_string(i), i));
    REQUIRE(memory.RememberAction("Matěj", "MOVE_FAILED [MOVE_TO: voda]", 33));
    REQUIRE(memory.RememberNpc(34, "Běla", "Nejnovější veřejná poznámka.", 34));
    CHECK(memory.Size() == 32);
    CHECK(HasMemory(memory.Retrieve(40, "voda"), "MOVE_FAILED [MOVE_TO: voda]"));
    CHECK_FALSE(HasMemory(memory.Retrieve(40, "jantar"), "Zapamatovaný jantar."));
}
