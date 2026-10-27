/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_NPCAGENTDIALOGUE_H
#define AIWORLD_NPCAGENTDIALOGUE_H

#include "AgentId.h"
#include "Define.h"
#include <algorithm>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace NpcAgentDialogueLimits
{
    inline constexpr std::size_t MaxTextBytes = 768;
    inline constexpr std::size_t MaxTextCharacters = 256;
    inline constexpr std::size_t MaxNameBytes = 80;
    inline constexpr std::size_t MaxNameCharacters = 32;
    inline constexpr std::size_t MaxHistoryLines = 12;
    inline constexpr std::size_t MaxConversations = 128;
    inline constexpr uint32 MaxHop = 3;
    inline constexpr uint32 MaxUtterances = 8;
    inline constexpr uint64 ConversationTtlMs = 60000;
    inline constexpr uint64 NpcCooldownMs = 2000;
}

// Strict UTF-8 scalar decoding: reject overlong forms, truncated sequences,
// surrogates, lone continuation bytes and values beyond Unicode's range.
inline bool ValidateNpcAgentPlainText(std::string_view text,
    std::size_t maxBytes = NpcAgentDialogueLimits::MaxTextBytes,
    std::size_t maxCharacters = NpcAgentDialogueLimits::MaxTextCharacters, bool allowAuthoredLines = false)
{
    if (text.empty() || text.size() > maxBytes)
        return false;

    std::size_t characters = 0;
    bool visible = false;
    for (std::size_t i = 0; i < text.size(); )
    {
        unsigned char first = static_cast<unsigned char>(text[i++]);
        uint32 scalar = first;
        uint32 continuation = 0;
        uint32 minimum = 0;
        if (first >= 0xC2 && first <= 0xDF)
        {
            scalar = first & 0x1F; continuation = 1; minimum = 0x80;
        }
        else if (first >= 0xE0 && first <= 0xEF)
        {
            scalar = first & 0x0F; continuation = 2; minimum = 0x800;
        }
        else if (first >= 0xF0 && first <= 0xF4)
        {
            scalar = first & 0x07; continuation = 3; minimum = 0x10000;
        }
        else if (first >= 0x80)
            return false;

        if (continuation > text.size() - i)
            return false;
        for (uint32 part = 0; part < continuation; ++part)
        {
            unsigned char next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xC0) != 0x80)
                return false;
            scalar = (scalar << 6) | (next & 0x3F);
        }

        if (scalar < minimum || scalar > 0x10FFFF || (scalar >= 0xD800 && scalar <= 0xDFFF))
            return false;
        // Pipe introduces WoW color, hyperlink, texture and escape markup.
        // Also exclude controls and invisible/bidirectional formatting so
        // a model response cannot forge chat rows or disguise such markup.
        bool authoredWhitespace = allowAuthoredLines && (scalar == '\n' || scalar == '\t');
        if ((!authoredWhitespace && scalar < 0x20) || (scalar >= 0x7F && scalar <= 0x9F) || scalar == '|'
            || (scalar >= 0x200B && scalar <= 0x200F) || (scalar >= 0x2028 && scalar <= 0x202E)
            || (scalar >= 0x2060 && scalar <= 0x206F) || scalar == 0xFEFF)
            return false;
        if (++characters > maxCharacters)
            return false;
        visible = visible || (!authoredWhitespace && scalar != ' ' && scalar != 0xA0);
    }
    return visible;
}

inline bool ValidateNpcAgentSpeechText(std::string_view text)
{
    return ValidateNpcAgentPlainText(text);
}

// Trim ordinary surrounding spaces only. Malformed or unsafe content is
// rejected as a whole, never repaired into a different spoken command.
inline std::string CleanNpcAgentSpeechText(std::string_view text)
{
    if (!ValidateNpcAgentSpeechText(text))
        return {};
    while (!text.empty() && text.front() == ' ')
        text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ')
        text.remove_suffix(1);
    return std::string(text);
}

struct NpcAgentSpeech
{
    uint64 ConversationId = 0;
    uint64 UtteranceId = 0;
    AgentId Speaker;
    std::string Kind = "NPC";
    std::string SpeakerName;
    std::string Text;
    uint64 AtMs = 0;
    uint32 Hop = 0;
};

inline bool ValidateNpcAgentSpeech(NpcAgentSpeech const& speech)
{
    return speech.ConversationId && speech.UtteranceId
        && (speech.Kind == "PLAYER" || speech.Kind == "NPC" || speech.Kind == "SELF")
        && (speech.Kind == "PLAYER" || bool(speech.Speaker))
        && speech.Hop <= NpcAgentDialogueLimits::MaxHop
        && ValidateNpcAgentPlainText(speech.SpeakerName, NpcAgentDialogueLimits::MaxNameBytes,
            NpcAgentDialogueLimits::MaxNameCharacters)
        && ValidateNpcAgentSpeechText(speech.Text);
}

// Each runtime agent owns its own instance. Remembering an overheard line
// does not grant access to any other agent's history or private profile.
class NpcAgentDialogueHistory
{
public:
    static constexpr std::size_t MaxLines = NpcAgentDialogueLimits::MaxHistoryLines;

    bool Remember(NpcAgentSpeech const& speech)
    {
        if (!ValidateNpcAgentSpeech(speech))
            return false;
        if (std::any_of(_lines.begin(), _lines.end(), [&](NpcAgentSpeech const& existing)
            { return existing.UtteranceId == speech.UtteranceId && existing.ConversationId == speech.ConversationId; }))
            return false;
        if (_lines.size() == MaxLines)
            _lines.pop_front();
        _lines.push_back(speech);
        _lines.back().Text = CleanNpcAgentSpeechText(speech.Text);
        return true;
    }

    std::deque<NpcAgentSpeech> const& Lines() const { return _lines; }
    void Clear() { _lines.clear(); }

private:
    std::deque<NpcAgentSpeech> _lines;
};

// One world-thread-owned guard for all NPC emissions. Admit exactly once
// before client delivery and listener fanout, never once per listener.
// Input from players belongs in histories but does not pass this guard.
class NpcAgentConversationGuard
{
public:
    static constexpr std::size_t MaxConversations = NpcAgentDialogueLimits::MaxConversations;
    static constexpr uint32 MaxHop = NpcAgentDialogueLimits::MaxHop;
    static constexpr uint32 MaxUtterances = NpcAgentDialogueLimits::MaxUtterances;
    static constexpr uint64 ConversationTtlMs = NpcAgentDialogueLimits::ConversationTtlMs;
    static constexpr uint64 NpcCooldownMs = NpcAgentDialogueLimits::NpcCooldownMs;

    bool Admit(NpcAgentSpeech const& speech, uint64 nowMs)
    {
        if (nowMs < _lastObservedMs)
            return false;
        Expire(nowMs);
        if (!ValidateNpcAgentSpeech(speech) || speech.Kind != "NPC" || !speech.Speaker
            || speech.AtMs > nowMs || nowMs - speech.AtMs >= ConversationTtlMs)
            return false;

        auto conversation = _conversations.find(speech.ConversationId);
        if (conversation != _conversations.end()
            && (conversation->second.Utterances.size() >= MaxUtterances
                || conversation->second.Utterances.count(speech.UtteranceId)))
            return false;
        if (conversation == _conversations.end() && _conversations.size() >= MaxConversations)
            return false;

        auto previous = _lastNpcSpeech.find(speech.Speaker.Value);
        if (previous != _lastNpcSpeech.end()
            && (nowMs < previous->second || nowMs - previous->second < NpcCooldownMs))
            return false;
        // Bound the independent cooldown index too. Its old rows expire
        // after two seconds; admitting at capacity fails without eviction.
        if (previous == _lastNpcSpeech.end() && _lastNpcSpeech.size() >= MaxConversations * MaxUtterances)
            return false;

        if (conversation == _conversations.end())
            conversation = _conversations.emplace(speech.ConversationId, Conversation{ nowMs, {} }).first;
        conversation->second.Utterances.insert(speech.UtteranceId);
        _lastNpcSpeech[speech.Speaker.Value] = nowMs;
        return true;
    }

    void Expire(uint64 nowMs)
    {
        // nowMs is the trusted world clock, independent of payload validity.
        // Retain its watermark even when an emission is rejected: expiry
        // must not let a later regressed call bypass a removed cooldown.
        if (nowMs < _lastObservedMs)
            return;
        _lastObservedMs = nowMs;
        for (auto it = _conversations.begin(); it != _conversations.end(); )
        {
            if (nowMs >= it->second.StartedAtMs && nowMs - it->second.StartedAtMs >= ConversationTtlMs)
                it = _conversations.erase(it);
            else
                ++it;
        }
        for (auto it = _lastNpcSpeech.begin(); it != _lastNpcSpeech.end(); )
        {
            if (nowMs >= it->second && nowMs - it->second >= NpcCooldownMs)
                it = _lastNpcSpeech.erase(it);
            else
                ++it;
        }
    }

    std::size_t Size() const { return _conversations.size(); }
    void Clear() { _conversations.clear(); _lastNpcSpeech.clear(); _lastObservedMs = 0; }

private:
    struct Conversation
    {
        uint64 StartedAtMs = 0;
        std::unordered_set<uint64> Utterances;
    };
    std::unordered_map<uint64, Conversation> _conversations;
    std::unordered_map<uint64, uint64> _lastNpcSpeech;
    uint64 _lastObservedMs = 0;
};

#endif // AIWORLD_NPCAGENTDIALOGUE_H
