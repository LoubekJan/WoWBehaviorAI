/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_NPCAGENTMEMORY_H
#define AIWORLD_NPCAGENTMEMORY_H

#include "Inference/NpcAgent.h"
#include "NpcAgentSpeechState.h"
#include <limits>
#include <set>

// Session-only, world-thread-owned evidence. Player quotations stay in their
// exact GUID scope; they are never public world facts or shared NPC knowledge.
class NpcAgentMemoryStore
{
public:
    static constexpr std::size_t MaxPrivateRecords = 64, MaxPublicRecords = 32;
    static constexpr std::size_t MaxRecordsPerPlayer = 32, MaxPlayers = 8;
    static constexpr std::size_t MaxRecalled = 6, MaxRecallBytes = 3072;

    bool RememberPlayer(uint64 turnId, ObjectGuid player, std::string const& speaker,
        std::string const& text, uint64 atMs)
    {
        if (!turnId || !player.IsPlayer() || !Valid(speaker, text) || !Observe(atMs)) return false;
        for (auto const& entry : _private)
            if (entry.TurnId == turnId && entry.Player == player) return false;
        if (!CanAllocate()) return false;
        std::vector<ObjectGuid> partners;
        for (auto const& entry : _private)
            if (std::find(partners.begin(), partners.end(), entry.Player) == partners.end()) partners.push_back(entry.Player);
        if (std::find(partners.begin(), partners.end(), player) == partners.end() && partners.size() >= MaxPlayers)
        {
            auto oldest = std::min_element(partners.begin(), partners.end(), [&](auto a, auto b)
                { return LastPlayerAt(a) < LastPlayerAt(b); });
            ObjectGuid evicted = *oldest;
            _private.erase(std::remove_if(_private.begin(), _private.end(), [&](auto const& entry)
                { return entry.Player == evicted; }), _private.end());
        }
        if (std::count_if(_private.begin(), _private.end(), [&](auto const& entry)
            { return entry.Player == player; }) >= MaxRecordsPerPlayer)
            EraseOldest(_private, player);
        if (_private.size() >= MaxPrivateRecords) EraseOldest(_private);
        _private.push_back({++_sequence, atMs, turnId, player, "PLAYER_DIALOGUE", speaker, CleanNpcAgentSpeechText(text), {}});
        return true;
    }

    // Called only after the named whisper was delivered. An unanswered or
    // superseded turn keeps its real incoming quote, without an invented reply.
    bool RememberPlayerReply(uint64 turnId, ObjectGuid player, std::string const& response, uint64 atMs)
    {
        if (!turnId || !player.IsPlayer() || !ValidateNpcAgentSpeechText(response) || !Observe(atMs)) return false;
        for (auto& entry : _private)
            if (entry.TurnId == turnId && entry.Player == player && entry.Response.empty())
            { entry.Response = CleanNpcAgentSpeechText(response); entry.AtMs = atMs; return true; }
        return false;
    }

    bool RememberNpc(uint64 utteranceId, std::string const& speaker, std::string const& text, uint64 atMs)
    {
        if (!utteranceId || !Valid(speaker, text) || !Observe(atMs)) return false;
        for (auto const& entry : _public)
            if (entry.Kind == "NPC_DIALOGUE" && entry.TurnId == utteranceId) return false;
        return Public("NPC_DIALOGUE", speaker, text, atMs, utteranceId);
    }

    // The controller supplies actual terminal engine feedback, never a model's
    // proposed action or claim of success. Repeated identical outcomes coalesce.
    bool RememberAction(std::string const& speaker, std::string const& feedback, uint64 atMs)
    {
        if (!Valid(speaker, feedback) || !Observe(atMs)) return false;
        for (auto& entry : _public)
            if (entry.Kind == "ACTION_RESULT" && entry.Speaker == speaker && entry.Text == feedback)
            { entry.AtMs = atMs; return true; }
        return Public("ACTION_RESULT", speaker, feedback, atMs, 0);
    }

    std::size_t Size() const { return _private.size() + _public.size(); }

    std::vector<NpcAgentMemory> Retrieve(uint64 nowMs, std::string const& query,
        ObjectGuid player = {}, uint64 excludedTurn = 0, std::string const& currentPlayerName = {}) const
    {
        if (!currentPlayerName.empty() && !ValidateNpcAgentPlainText(currentPlayerName, 80, 32)) return {};
        struct Candidate { Record const* Entry; uint32 Score; };
        std::vector<Candidate> candidates;
        auto queryTokens = Tokens(query);
        auto offer = [&](Record const& entry, bool personal)
        {
            if (entry.AtMs > nowMs || (personal && excludedTurn && entry.TurnId == excludedTurn)) return;
            auto tokens = Tokens(entry.Text);
            auto responseTokens = Tokens(entry.Response); tokens.insert(responseTokens.begin(), responseTokens.end());
            auto speakerTokens = Tokens(entry.Speaker); tokens.insert(speakerTokens.begin(), speakerTokens.end());
            uint32 matches = 0;
            for (auto const& word : queryTokens)
                if (std::any_of(tokens.begin(), tokens.end(), [&](auto const& token) { return Related(word, token); })) ++matches;
            // Relevance dominates recency. Personal recollections receive a
            // modest preference over unrelated generic action outcomes.
            candidates.push_back({&entry, matches * 1000u + (personal ? 200u : 0u)});
        };
        for (auto const& entry : _public) offer(entry, false);
        if (player.IsPlayer())
            for (auto const& entry : _private) if (entry.Player == player) offer(entry, true);
        std::sort(candidates.begin(), candidates.end(), [](auto const& a, auto const& b)
        {
            if (a.Score != b.Score) return a.Score > b.Score;
            if (a.Entry->AtMs != b.Entry->AtMs) return a.Entry->AtMs > b.Entry->AtMs;
            return a.Entry->Id > b.Entry->Id;
        });
        std::vector<Candidate> selected;
        std::size_t bytes = 0;
        for (auto const& candidate : candidates)
        {
            auto const& entry = *candidate.Entry;
            std::size_t speakerBytes = !entry.Player.IsEmpty() && !currentPlayerName.empty() ? currentPlayerName.size() : entry.Speaker.size();
            std::size_t cost = speakerBytes + entry.Text.size() + entry.Response.size();
            if (cost > MaxRecallBytes - bytes) continue;
            selected.push_back(candidate); bytes += cost;
            if (selected.size() == MaxRecalled) break;
        }
        std::sort(selected.begin(), selected.end(), [](auto const& a, auto const& b)
        { return a.Entry->AtMs == b.Entry->AtMs ? a.Entry->Id < b.Entry->Id : a.Entry->AtMs < b.Entry->AtMs; });
        std::vector<NpcAgentMemory> result;
        for (auto const& candidate : selected)
        {
            auto const& entry = *candidate.Entry;
            NpcAgentMemory memory;
            memory.Id = entry.Id; memory.AgeMs = nowMs - entry.AtMs;
            memory.Kind = entry.Kind;
            memory.Speaker = !entry.Player.IsEmpty() && !currentPlayerName.empty() ? currentPlayerName : entry.Speaker;
            memory.Text = entry.Text; memory.Response = entry.Response;
            memory.Relevance = candidate.Score; result.push_back(std::move(memory));
        }
        return result;
    }

private:
    struct Record
    {
        uint64 Id, AtMs, TurnId;
        ObjectGuid Player;
        std::string Kind, Speaker, Text, Response;
    };
    std::deque<Record> _private, _public;
    uint64 _sequence = 0, _observedAtMs = 0;

    static bool Valid(std::string const& speaker, std::string const& text)
    { return ValidateNpcAgentPlainText(speaker, 80, 32) && ValidateNpcAgentSpeechText(text); }
    bool Observe(uint64 atMs)
    { if (atMs < _observedAtMs) return false; _observedAtMs = atMs; return true; }
    bool CanAllocate() const { return _sequence < std::numeric_limits<uint64>::max(); }
    static void EraseOldest(std::deque<Record>& entries, ObjectGuid player = {})
    {
        auto oldest = entries.end();
        for (auto it = entries.begin(); it != entries.end(); ++it)
            if ((player.IsEmpty() || it->Player == player) && (oldest == entries.end() ||
                it->AtMs < oldest->AtMs || (it->AtMs == oldest->AtMs && it->Id < oldest->Id))) oldest = it;
        if (oldest != entries.end()) entries.erase(oldest);
    }
    uint64 LastPlayerAt(ObjectGuid player) const
    { uint64 last = 0; for (auto const& entry : _private) if (entry.Player == player) last = std::max(last, entry.AtMs); return last; }
    bool Public(std::string kind, std::string const& speaker, std::string const& text, uint64 atMs, uint64 turnId)
    {
        if (!CanAllocate()) return false;
        if (_public.size() == MaxPublicRecords) EraseOldest(_public);
        _public.push_back({++_sequence, atMs, turnId, {}, std::move(kind), speaker, CleanNpcAgentSpeechText(text), {}});
        return true;
    }
    static std::set<std::string> Tokens(std::string text)
    {
        // Treat ASCII punctuation as word boundaries before shared UTF-8/Czech
        // case folding. No byte slicing or generated semantic facts are used.
        for (char& c : text)
            if ((c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') || (c >= '{' && c <= '~')) c = ' ';
        text = NormalizeNpcAgentSpeech(text);
        static std::set<std::string> const noise{"jsem", "jsi", "jste", "nebo", "jako", "který", "která", "které",
            "jakou", "bylo", "byla", "byli", "mám", "máš", "proč", "kdy", "kde", "jak", "tak", "ano", "the", "and", "you", "your", "what"};
        std::set<std::string> result;
        std::size_t from = 0;
        while (from < text.size())
        {
            auto end = text.find(' ', from); if (end == std::string::npos) end = text.size();
            auto word = text.substr(from, end - from);
            if (word.size() >= 3 && !noise.count(word)) result.insert(std::move(word));
            from = end + 1;
        }
        return result;
    }
    static bool Related(std::string const& a, std::string const& b)
    {
        if (a == b) return true;
        // A small lexical prefix tolerance handles common Czech inflection
        // (barva/barvu). It only ranks evidence; it never changes that evidence.
        std::size_t shared = 0;
        while (shared < a.size() && shared < b.size() && a[shared] == b[shared]) ++shared;
        while (shared && shared < a.size() && (static_cast<unsigned char>(a[shared]) & 0xC0) == 0x80) --shared;
        return shared >= 4 && a.size() - shared <= 4 && b.size() - shared <= 4;
    }
};
#endif
