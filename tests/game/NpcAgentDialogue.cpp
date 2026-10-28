/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/NpcAgentDialogue.h"
#include <string>

namespace
{
    NpcAgentSpeech Line(uint64 id, uint64 conversation = 1, uint64 speaker = 901035, uint64 atMs = 1000)
    {
        NpcAgentSpeech speech;
        speech.ConversationId = conversation;
        speech.UtteranceId = id;
        speech.Speaker = AgentId{ speaker };
        speech.SpeakerName = "Matej";
        speech.Text = "Mam hlad, zajdu do skladu.";
        speech.AtMs = atMs;
        speech.Hop = 1;
        return speech;
    }
}

TEST_CASE("NPC speech accepts Unicode scalar text and trims only surrounding spaces", "[AIWorld][NpcAgent]")
{
    CHECK(ValidateNpcAgentSpeechText("B\xC4\x9Bla m\xC3\xA1 hlad."));
    CHECK(ValidateNpcAgentSpeechText("\xF0\x9F\x8D\x9E"));
    CHECK(CleanNpcAgentSpeechText("  Ahoj, sousede.  ") == "Ahoj, sousede.");
    CHECK(ValidateNpcAgentSpeechText(std::string(256, 'a')));
    CHECK_FALSE(ValidateNpcAgentSpeechText(std::string(257, 'a')));
    std::string threeByte;
    for (std::size_t i = 0; i < 256; ++i)
        threeByte += "\xE4\xB8\xAD";
    CHECK(threeByte.size() == NpcAgentDialogueLimits::MaxTextBytes);
    CHECK(ValidateNpcAgentSpeechText(threeByte));
    threeByte += "a";
    CHECK_FALSE(ValidateNpcAgentSpeechText(threeByte));
    std::string fourByte;
    for (std::size_t i = 0; i < 193; ++i)
        fourByte += "\xF0\x9F\x8D\x9E";
    CHECK_FALSE(ValidateNpcAgentSpeechText(fourByte));
}

TEST_CASE("NPC speech rejects malformed UTF8 controls and WoW packet markup", "[AIWorld][NpcAgent]")
{
    for (std::string const& invalid : {
        std::string{}, std::string("   "), std::string("a\nb"), std::string("a\tb"),
        std::string("a\0b", 3), std::string("\x7F"), std::string("\xC2\x85"),
        std::string("\x80"), std::string("\xC0\xAF"), std::string("\xE0\x80\xAF"),
        std::string("\xF0\x80\x80\xAF"), std::string("\xED\xA0\x80"),
        std::string("\xF4\x90\x80\x80"), std::string("\xF0\x9F"),
        std::string("\xE2\x80\xAEtext"), std::string("\xEF\xBB\xBFtext"),
        std::string("|cffff0000fake|r"), std::string("|Hitem:1|hthing|h"),
        std::string("|Tpath:20|t"), std::string("literal||pipe") })
    {
        CHECK_FALSE(ValidateNpcAgentSpeechText(invalid));
        CHECK(CleanNpcAgentSpeechText(invalid).empty());
    }
}

TEST_CASE("NPC history is private bounded copied and replay resistant", "[AIWorld][NpcAgent]")
{
    NpcAgentDialogueHistory first;
    NpcAgentDialogueHistory second;
    auto playerLine = Line(1);
    playerLine.Kind = "PLAYER";
    playerLine.Speaker = {};
    playerLine.Text = "  Ahoj.  ";
    REQUIRE(first.Remember(playerLine));
    CHECK(first.Lines().front().Text == "Ahoj.");
    playerLine.Text = "Changed outside history";
    CHECK(first.Lines().front().Text == "Ahoj.");
    CHECK(second.Lines().empty());
    CHECK_FALSE(first.Remember(playerLine));
    for (uint64 id = 2; id <= 15; ++id)
        REQUIRE(first.Remember(Line(id)));
    REQUIRE(first.Lines().size() == NpcAgentDialogueHistory::MaxLines);
    CHECK(first.Lines().front().UtteranceId == 4);
    CHECK(first.Lines().back().UtteranceId == 15);
    REQUIRE(second.Remember(Line(1)));
    first.Clear();
    CHECK(first.Lines().empty());
    CHECK(second.Lines().size() == 1);
    auto invalid = Line(16);
    invalid.Kind = "SYSTEM";
    CHECK_FALSE(first.Remember(invalid));
    invalid = Line(17);
    invalid.Speaker = {};
    CHECK_FALSE(first.Remember(invalid));
}

TEST_CASE("NPC outgoing speech guard enforces hop cooldown and eight utterance conversation budgets", "[AIWorld][NpcAgent]")
{
    NpcAgentConversationGuard guard;
    auto speech = Line(1);
    speech.Hop = NpcAgentConversationGuard::MaxHop + 1;
    CHECK_FALSE(guard.Admit(speech, 1000));
    speech.Hop = NpcAgentConversationGuard::MaxHop;
    REQUIRE(guard.Admit(speech, 1000));
    CHECK_FALSE(guard.Admit(speech, 3000));
    speech = Line(2, 2, 901035, 2999);
    CHECK_FALSE(guard.Admit(speech, 2999));
    speech.AtMs = 3000;
    REQUIRE(guard.Admit(speech, 3000));
    // A failed cooldown check must not spend an utterance or speaker turn.
    for (uint64 id = 3; id <= 9; ++id)
    {
        auto next = Line(id, 1, 901035, id * 2000);
        REQUIRE(guard.Admit(next, next.AtMs));
    }
    auto ninth = Line(10, 1, 901036, 20000);
    CHECK_FALSE(guard.Admit(ninth, ninth.AtMs));
    auto other = Line(11, 3, 901036, 20000);
    CHECK(guard.Admit(other, other.AtMs));
}

TEST_CASE("NPC outgoing guard bounds active conversations and expires them without evicting live budgets", "[AIWorld][NpcAgent]")
{
    NpcAgentConversationGuard guard;
    for (uint64 id = 1; id <= NpcAgentConversationGuard::MaxConversations; ++id)
        REQUIRE(guard.Admit(Line(id, id, 900000 + id), 1000));
    CHECK(guard.Size() == NpcAgentConversationGuard::MaxConversations);
    CHECK_FALSE(guard.Admit(Line(999, 999, 999999), 1000));
    guard.Expire(1000 + NpcAgentConversationGuard::ConversationTtlMs - 1);
    CHECK(guard.Size() == NpcAgentConversationGuard::MaxConversations);
    guard.Expire(1000 + NpcAgentConversationGuard::ConversationTtlMs);
    CHECK(guard.Size() == 0);
    CHECK_FALSE(guard.Admit(Line(1000, 1000, 999999), 61000));
    REQUIRE(guard.Admit(Line(1001, 1001, 999999, 61000), 61000));
    guard.Clear();
    CHECK(guard.Size() == 0);
}

TEST_CASE("NPC outgoing guard refuses forged identities future timestamps and unsafe content", "[AIWorld][NpcAgent]")
{
    NpcAgentConversationGuard guard;
    auto speech = Line(1);
    speech.Kind = "PLAYER";
    CHECK_FALSE(guard.Admit(speech, 1000));
    speech = Line(2);
    speech.Speaker = {};
    CHECK_FALSE(guard.Admit(speech, 1000));
    speech = Line(3);
    CHECK_FALSE(guard.Admit(speech, 999));
    speech.Text = "|cffffffffspoof|r";
    CHECK_FALSE(guard.Admit(speech, 1000));
    speech = Line(4);
    speech.ConversationId = 0;
    CHECK_FALSE(guard.Admit(speech, 1000));
    speech = Line(0);
    CHECK_FALSE(guard.Admit(speech, 1000));
    REQUIRE(guard.Admit(Line(5), 1000));
    CHECK_FALSE(guard.Admit(Line(6, 1, 901035, 900), 900));
}

TEST_CASE("NPC outgoing guard retains its clock watermark after expiry and rejected emissions", "[AIWorld][NpcAgent]")
{
    NpcAgentConversationGuard guard;
    REQUIRE(guard.Admit(Line(1), 1000));
    // This duplicate advances trusted time and expires the cooldown row,
    // but must not permit a following call to move the clock backwards.
    CHECK_FALSE(guard.Admit(Line(1), 3000));
    CHECK_FALSE(guard.Admit(Line(2, 1, 901035, 2999), 2999));
    REQUIRE(guard.Admit(Line(2, 1, 901035, 3000), 3000));
    guard.Expire(100000);
    CHECK(guard.Size() == 0);
    guard.Expire(5000);
    CHECK_FALSE(guard.Admit(Line(3, 2, 901036, 99999), 99999));
    REQUIRE(guard.Admit(Line(3, 2, 901036, 100000), 100000));
    guard.Clear();
    REQUIRE(guard.Admit(Line(4, 3, 901035, 1000), 1000));
}
