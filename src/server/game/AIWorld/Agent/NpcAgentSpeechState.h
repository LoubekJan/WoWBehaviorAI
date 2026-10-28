/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_NPCAGENTSPEECHSTATE_H
#define AIWORLD_NPCAGENTSPEECHSTATE_H

#include "NpcAgentDialogue.h"
#include "ObjectGuid.h"
#include <optional>
#include <vector>

struct NpcAgentPendingTurn
{
    uint64 Id = 0;
    ObjectGuid Partner;
    std::string Name, Text;
    uint64 AtMs = 0;
};

// Case and punctuation changes must not turn the same ambient line into a new
// utterance. Decode scalars so Czech case folding never corrupts UTF-8.
inline std::string NormalizeNpcAgentSpeech(std::string_view text)
{
    if (!ValidateNpcAgentSpeechText(text)) return {};
    std::string result;
    bool space = false;
    for (std::size_t i = 0; i < text.size(); )
    {
        uint32 cp = static_cast<unsigned char>(text[i++]);
        unsigned extra = 0;
        if (cp >= 0xF0) { cp &= 7; extra = 3; }
        else if (cp >= 0xE0) { cp &= 15; extra = 2; }
        else if (cp >= 0xC0) { cp &= 31; extra = 1; }
        while (extra--) cp = (cp << 6) | (static_cast<unsigned char>(text[i++]) & 63);
        if (cp == ' ' || cp == 0xA0) { space = !result.empty(); continue; }
        if ((cp >= 33 && cp <= 47) || (cp >= 58 && cp <= 64) || (cp >= 91 && cp <= 96) ||
            (cp >= 123 && cp <= 126) || (cp >= 0x2010 && cp <= 0x201F) || cp == 0x2026) continue;
        if (cp >= 'A' && cp <= 'Z') cp += 32;
        else if ((cp >= 0xC0 && cp <= 0xD6) || (cp >= 0xD8 && cp <= 0xDE)) cp += 32;
        else if ((cp >= 0x100 && cp <= 0x12F && !(cp & 1)) ||
            (cp >= 0x132 && cp <= 0x137 && !(cp & 1)) ||
            (cp >= 0x139 && cp <= 0x148 && (cp & 1)) ||
            (cp >= 0x14A && cp <= 0x177 && !(cp & 1)) ||
            (cp >= 0x179 && cp <= 0x17E && (cp & 1))) ++cp;
        if (space) { result += ' '; space = false; }
        if (cp < 0x80) result += char(cp);
        else if (cp < 0x800) { result += char(0xC0 | (cp >> 6)); result += char(0x80 | (cp & 63)); }
        else if (cp < 0x10000)
        { result += char(0xE0 | (cp >> 12)); result += char(0x80 | ((cp >> 6) & 63)); result += char(0x80 | (cp & 63)); }
        else
        { result += char(0xF0 | (cp >> 18)); result += char(0x80 | ((cp >> 12) & 63)); result += char(0x80 | ((cp >> 6) & 63)); result += char(0x80 | (cp & 63)); }
    }
    return result;
}

// World-thread state only. Private turns/history never enter the shared NPC
// transcript. Admission is checked before sending; CommitSent is called only
// after the chat bridge reports an actual delivery.
struct NpcAgentSpeechState
{
    static constexpr uint64 PlayerTurnTtlMs = 60000, NpcTurnTtlMs = 15000;
    static constexpr uint64 AmbientIntervalMs = 60000, RepeatWindowMs = 120000;
    static constexpr std::size_t MaxPrivatePartners = 4, MaxRecentAmbient = 8;
    std::optional<NpcAgentPendingTurn> PlayerTurn, NpcTurn;

    bool PinPlayer(NpcAgentPendingTurn turn)
    {
        if (!ValidTurn(turn) || !turn.Partner.IsPlayer()) return false;
        if (PlayerTurn && PlayerTurn->Id == turn.Id) return false;
        auto* history = FindPrivate(turn.Partner);
        if (!history)
        {
            if (_private.size() == MaxPrivatePartners)
                _private.erase(std::min_element(_private.begin(), _private.end(),
                    [](auto const& a, auto const& b) { return a.LastAtMs < b.LastAtMs; }));
            _private.push_back({turn.Partner, {}, turn.AtMs}); history = &_private.back();
        }
        NpcAgentSpeech line;
        line.ConversationId = line.UtteranceId = turn.Id; line.SpeakerName = turn.Name;
        line.Text = turn.Text; line.Kind = "PLAYER"; line.AtMs = turn.AtMs;
        if (!history->History.Remember(line)) return false;
        history->LastAtMs = turn.AtMs;
        PlayerTurn = std::move(turn); NpcTurn.reset();
        return true;
    }

    bool PinNpc(NpcAgentPendingTurn turn, uint64 nowMs)
    {
        if (PlayerTurn || !ValidTurn(turn) || !turn.Partner.IsCreature() ||
            !Fresh(turn, nowMs, NpcTurnTtlMs) || !CanAmbient(nowMs)) return false;
        NpcTurn = std::move(turn);
        return true;
    }

    bool Expire(uint64 nowMs, bool playerConnected = true)
    {
        if (!Observe(nowMs)) return false;
        bool changed = false;
        if (PlayerTurn && (!playerConnected || !Fresh(*PlayerTurn, nowMs, PlayerTurnTtlMs)))
        { PlayerTurn.reset(); changed = true; }
        if (NpcTurn && !Fresh(*NpcTurn, nowMs, NpcTurnTtlMs)) { NpcTurn.reset(); changed = true; }
        return changed;
    }

    NpcAgentDialogueHistory const* PrivateHistory(ObjectGuid partner) const
    {
        for (auto const& conversation : _private)
            if (conversation.Partner == partner) return &conversation.History;
        return nullptr;
    }

    bool CanAmbient(uint64 nowMs, std::string_view text = {})
    {
        if (!Observe(nowMs) || (_hasSpoken && nowMs - _lastSpeechAtMs < AmbientIntervalMs)) return false;
        if (text.empty()) return true;
        auto normalized = NormalizeNpcAgentSpeech(text);
        if (normalized.empty()) return false;
        for (auto const& sent : _recentAmbient)
            if (nowMs >= sent.AtMs && nowMs - sent.AtMs < RepeatWindowMs && sent.Text == normalized) return false;
        return true;
    }

    bool CanSend(std::string_view mode, uint64 turnId, ObjectGuid partner, std::string_view text, uint64 nowMs)
    {
        if (!Observe(nowMs) || !ValidateNpcAgentSpeechText(text)) return false;
        if (mode == "PLAYER_REPLY") return Matches(PlayerTurn, turnId, partner, nowMs, PlayerTurnTtlMs);
        if (mode == "NPC_REPLY") return !PlayerTurn && Matches(NpcTurn, turnId, partner, nowMs, NpcTurnTtlMs) && CanAmbient(nowMs, text);
        return mode == "INITIATE" && !PlayerTurn && !NpcTurn && !turnId && partner.IsEmpty() && CanAmbient(nowMs, text);
    }

    bool CommitSent(std::string_view mode, uint64 turnId, ObjectGuid partner, NpcAgentSpeech const& line, uint64 nowMs)
    {
        if (!CanSend(mode, turnId, partner, line.Text, nowMs)) return false;
        if (mode == "PLAYER_REPLY")
        {
            if (auto* history = FindPrivate(partner))
            { auto own = line; own.Kind = "SELF"; history->History.Remember(own); history->LastAtMs = nowMs; }
            PlayerTurn.reset();
        }
        else
        {
            if (mode == "NPC_REPLY") NpcTurn.reset();
        }
        // Private replies are allowed to repeat for a new explicit question,
        // but must not be replayed later as an autonomous public remark.
        while (!_recentAmbient.empty() && nowMs - _recentAmbient.front().AtMs >= RepeatWindowMs) _recentAmbient.pop_front();
        if (_recentAmbient.size() == MaxRecentAmbient) _recentAmbient.pop_front();
        _recentAmbient.push_back({NormalizeNpcAgentSpeech(line.Text), nowMs});
        _hasSpoken = true; _lastSpeechAtMs = nowMs;
        return true;
    }

    bool CanInitiate(uint64 scene, bool audience, uint64 nowMs)
    { return !PlayerTurn && !NpcTurn && audience && CanAmbient(nowMs) && (!_submittedScene || *_submittedScene != scene); }
    void SubmittedInitiation(uint64 scene) { _submittedScene = scene; }

private:
    struct PrivateConversation { ObjectGuid Partner; NpcAgentDialogueHistory History; uint64 LastAtMs = 0; };
    struct SentLine { std::string Text; uint64 AtMs = 0; };
    std::vector<PrivateConversation> _private;
    std::deque<SentLine> _recentAmbient;
    std::optional<uint64> _submittedScene;
    uint64 _lastSpeechAtMs = 0, _observedAtMs = 0;
    bool _hasSpoken = false;
    bool Observe(uint64 nowMs)
    { if (nowMs < _observedAtMs) return false; _observedAtMs = nowMs; return true; }
    PrivateConversation* FindPrivate(ObjectGuid partner)
    { for (auto& conversation : _private) if (conversation.Partner == partner) return &conversation; return nullptr; }
    static bool ValidTurn(NpcAgentPendingTurn const& turn)
    { return turn.Id && !turn.Partner.IsEmpty() && ValidateNpcAgentPlainText(turn.Name, 80, 32) && ValidateNpcAgentSpeechText(turn.Text); }
    static bool Fresh(NpcAgentPendingTurn const& turn, uint64 nowMs, uint64 ttl)
    { return nowMs >= turn.AtMs && nowMs - turn.AtMs < ttl; }
    static bool Matches(std::optional<NpcAgentPendingTurn> const& turn, uint64 id, ObjectGuid partner, uint64 nowMs, uint64 ttl)
    { return turn && id && id == turn->Id && partner == turn->Partner && Fresh(*turn, nowMs, ttl); }
};

#endif
