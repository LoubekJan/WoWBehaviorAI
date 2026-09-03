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

#include "DynamicTaskJsonCodec.h"
#include "QuestContractLimits.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>

namespace
{
    // ---------------------------------------------------------------
    // Serialize side
    // ---------------------------------------------------------------

    // Appends `value` as a properly escaped, quoted JSON string. Handles
    // '"', '\\', the short control-character escapes, and escapes every
    // other byte < 0x20 as \u00XX - the one string field this codec ever
    // writes (QuestTargetCandidate::DisplayName) is server-resolved
    // display text, not attacker input, but it is still free text (may
    // contain quotes, e.g. a creature name), so it gets the same
    // treatment a genuinely untrusted string would.
    void AppendEscapedJsonString(std::ostringstream& out, std::string const& value)
    {
        out << '"';
        for (unsigned char c : value)
        {
            switch (c)
            {
                case '"':  out << "\\\""; break;
                case '\\': out << "\\\\"; break;
                case '\b': out << "\\b"; break;
                case '\f': out << "\\f"; break;
                case '\n': out << "\\n"; break;
                case '\r': out << "\\r"; break;
                case '\t': out << "\\t"; break;
                default:
                    if (c < 0x20)
                    {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                        out << buf;
                    }
                    else
                        out << static_cast<char>(c);
                    break;
            }
        }
        out << '"';
    }

    std::string SerializeQuestProblemContext(QuestProblemContext const& problem)
    {
        std::ostringstream body;
        body << "{\"type\":\"" << ToString(problem.Type) << "\""
             << ",\"actor_entry\":" << problem.ActorEntry
             << ",\"target_entry\":" << problem.TargetEntry
             << ",\"map_id\":" << problem.MapId
             << ",\"age_ms\":" << problem.AgeMs
             << "}";
        return body.str();
    }

    std::string SerializeQuestRelevantEvent(QuestRelevantEvent const& event)
    {
        std::ostringstream body;
        body << "{\"type\":\"" << ToString(event.Type) << "\""
             << ",\"actor_entry\":" << event.ActorEntry
             << ",\"target_entry\":" << event.TargetEntry
             << ",\"importance\":" << event.Importance
             << ",\"relevance\":" << event.Relevance
             << ",\"age_ms\":" << event.AgeMs
             << "}";
        return body.str();
    }

    std::string SerializeQuestTargetCandidate(QuestTargetCandidate const& candidate)
    {
        std::ostringstream body;
        body << "{\"token\":" << candidate.Token
             << ",\"entry\":" << candidate.Entry
             << ",\"display_name\":";
        AppendEscapedJsonString(body, candidate.DisplayName);
        body << ",\"map_id\":" << candidate.MapId
             << ",\"distance_yards\":" << candidate.DistanceYards
             << ",\"observation_age_ms\":" << candidate.ObservationAgeMs
             << "}";
        return body.str();
    }

    std::string SerializeQuestProposalLimits(QuestProposalLimits const& limits)
    {
        std::ostringstream body;
        body << "{\"max_required_count\":" << limits.MaxRequiredCount
             << ",\"max_range_yards\":" << limits.MaxRangeYards
             << ",\"max_expiry_ms\":" << limits.MaxExpiryMs
             << ",\"max_reward_money_copper\":" << limits.MaxRewardMoneyCopper
             << "}";
        return body.str();
    }

    std::string SerializeQuestContext(QuestContext const& context)
    {
        std::ostringstream body;
        body << "{\"agent_id\":" << context.Agent.Value
             << ",\"snapshot_sequence\":" << context.SnapshotSequence
             << ",\"problem\":" << SerializeQuestProblemContext(context.Problem)
             << ",\"relevant_events\":[";

        for (std::size_t i = 0; i < context.RelevantEvents.size(); ++i)
        {
            if (i > 0)
                body << ",";
            body << SerializeQuestRelevantEvent(context.RelevantEvents[i]);
        }

        body << "],\"candidate_targets\":[";

        for (std::size_t i = 0; i < context.CandidateTargets.size(); ++i)
        {
            if (i > 0)
                body << ",";
            body << SerializeQuestTargetCandidate(context.CandidateTargets[i]);
        }

        body << "],\"limits\":" << SerializeQuestProposalLimits(context.Limits)
             << "}";
        return body.str();
    }

    // ---------------------------------------------------------------
    // Parse side
    // ---------------------------------------------------------------

    constexpr std::size_t NPos = std::string_view::npos;

    // Returns the index just past the closing, unescaped '"' of the
    // string literal starting at json[start] (which must itself be '"'),
    // or NPos if the literal is unterminated or ends mid-escape. Steps
    // over every backslash-escape pair as a unit so an escaped quote
    // (\") is never mistaken for the closing quote.
    std::size_t SkipJsonStringLiteral(std::string_view json, std::size_t start)
    {
        if (start >= json.size() || json[start] != '"')
            return NPos;

        std::size_t i = start + 1;
        while (i < json.size())
        {
            char c = json[i];
            if (c == '\\')
            {
                if (i + 1 >= json.size())
                    return NPos;
                i += 2;
                continue;
            }
            if (c == '"')
                return i + 1;
            ++i;
        }
        return NPos;
    }

    // Unescapes `literal` (the full quoted span, including both
    // surrounding quotes, exactly as returned by SkipJsonStringLiteral)
    // into `out`. Supports \" \\ \/ \b \f \n \r \t and \uXXXX (BMP only -
    // no surrogate-pair handling; sufficient for the plain text this
    // schema ever carries). Returns false on any malformed escape.
    bool UnescapeJsonString(std::string_view literal, std::string& out)
    {
        if (literal.size() < 2 || literal.front() != '"' || literal.back() != '"')
            return false;

        std::size_t contentEnd = literal.size() - 1; // index of the closing quote
        out.clear();

        for (std::size_t i = 1; i < contentEnd; )
        {
            char c = literal[i];
            if (c != '\\')
            {
                out.push_back(c);
                ++i;
                continue;
            }

            if (i + 1 >= contentEnd)
                return false; // trailing backslash with nothing after it

            char esc = literal[i + 1];
            switch (esc)
            {
                case '"':  out.push_back('"');  i += 2; break;
                case '\\': out.push_back('\\'); i += 2; break;
                case '/':  out.push_back('/');  i += 2; break;
                case 'b':  out.push_back('\b'); i += 2; break;
                case 'f':  out.push_back('\f'); i += 2; break;
                case 'n':  out.push_back('\n'); i += 2; break;
                case 'r':  out.push_back('\r'); i += 2; break;
                case 't':  out.push_back('\t'); i += 2; break;
                case 'u':
                {
                    if (i + 6 > contentEnd)
                        return false;

                    unsigned codepoint = 0;
                    for (int k = 0; k < 4; ++k)
                    {
                        char h = literal[i + 2 + k];
                        codepoint <<= 4;
                        if (h >= '0' && h <= '9')
                            codepoint |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f')
                            codepoint |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F')
                            codepoint |= static_cast<unsigned>(h - 'A' + 10);
                        else
                            return false;
                    }

                    // Minimal UTF-8 encode (BMP only, no surrogate-pair
                    // combining - a lone surrogate is encoded as-is
                    // rather than rejected, matching a tolerant decoder
                    // degrading gracefully rather than failing the whole
                    // parse over one unpaired surrogate).
                    if (codepoint <= 0x7F)
                        out.push_back(static_cast<char>(codepoint));
                    else if (codepoint <= 0x7FF)
                    {
                        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
                        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                    }
                    else
                    {
                        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                    }

                    i += 6;
                    break;
                }
                default:
                    return false; // unknown escape
            }
        }

        return true;
    }

    // Scans a raw JSON number token (optional leading '-', digits,
    // optional '.'+digits, optional exponent) starting at `start`.
    // Returns the exclusive end index, or exactly `start` if no integer
    // digit was found at all (rejects a bare sign/decimal point with no
    // digits rather than "succeeding" with an empty token).
    std::size_t ScanJsonNumberToken(std::string_view json, std::size_t start)
    {
        std::size_t i = start;
        if (i < json.size() && json[i] == '-')
            ++i;

        std::size_t digitsStart = i;
        while (i < json.size() && std::isdigit(static_cast<unsigned char>(json[i])))
            ++i;
        if (i == digitsStart)
            return start;

        if (i < json.size() && json[i] == '.')
        {
            std::size_t j = i + 1;
            std::size_t fracStart = j;
            while (j < json.size() && std::isdigit(static_cast<unsigned char>(json[j])))
                ++j;
            if (j > fracStart)
                i = j;
        }

        if (i < json.size() && (json[i] == 'e' || json[i] == 'E'))
        {
            std::size_t j = i + 1;
            if (j < json.size() && (json[j] == '+' || json[j] == '-'))
                ++j;
            std::size_t expDigitsStart = j;
            while (j < json.size() && std::isdigit(static_cast<unsigned char>(json[j])))
                ++j;
            if (j > expDigitsStart)
                i = j;
        }

        return i;
    }

    // Locates `"key":` at or after `searchFrom` and returns the index of
    // the first non-whitespace character of its value (the start of
    // whatever comes after the colon). Returns NPos if the key isn't
    // found at or after searchFrom, or has no ':' after it.
    //
    // Deliberately searches forward-only from a caller-supplied cursor
    // rather than always from 0: every Find*Field/FindJsonValueSpan call
    // in this file is used to walk this schema's fields in their known,
    // fixed emission order (see ParseDynamicTaskResponse), threading each
    // field's own end position into the next search. That makes a
    // collision between a key name and literal text inside an earlier
    // untrusted string value (title/description) structurally impossible
    // here: the search for a later field can never match anything at or
    // before the end of an earlier one.
    std::size_t FindFieldValueStart(std::string_view json, std::string_view key, std::size_t searchFrom)
    {
        std::string needle;
        needle.reserve(key.size() + 2);
        needle.push_back('"');
        needle.append(key);
        needle.push_back('"');

        std::size_t keyPos = json.find(needle, searchFrom);
        if (keyPos == NPos)
            return NPos;

        std::size_t colonPos = json.find(':', keyPos + needle.size());
        if (colonPos == NPos)
            return NPos;

        std::size_t i = colonPos + 1;
        while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i])))
            ++i;

        return i;
    }

    bool FindJsonStringField(std::string_view json, std::string_view key, std::size_t searchFrom, std::string& out, std::size_t& valueEnd)
    {
        std::size_t valueStart = FindFieldValueStart(json, key, searchFrom);
        if (valueStart == NPos || valueStart >= json.size() || json[valueStart] != '"')
            return false;

        std::size_t literalEnd = SkipJsonStringLiteral(json, valueStart);
        if (literalEnd == NPos)
            return false;

        if (!UnescapeJsonString(json.substr(valueStart, literalEnd - valueStart), out))
            return false;

        valueEnd = literalEnd;
        return true;
    }

    bool FindJsonUInt64Field(std::string_view json, std::string_view key, std::size_t searchFrom, uint64& out, std::size_t& valueEnd)
    {
        std::size_t valueStart = FindFieldValueStart(json, key, searchFrom);
        if (valueStart == NPos)
            return false;

        std::size_t end = ScanJsonNumberToken(json, valueStart);
        if (end == valueStart)
            return false;

        std::string token(json.substr(valueStart, end - valueStart));
        // A wire uint field must be a plain non-negative integer literal -
        // no sign, no fraction, no exponent. Reject anything else rather
        // than silently truncating/rounding it into range.
        if (token.find_first_of("-.eE") != std::string::npos)
            return false;

        try
        {
            std::size_t consumed = 0;
            unsigned long long value = std::stoull(token, &consumed);
            if (consumed != token.size())
                return false;
            out = static_cast<uint64>(value);
        }
        catch (std::exception const&)
        {
            return false;
        }

        valueEnd = end;
        return true;
    }

    bool FindJsonFloatField(std::string_view json, std::string_view key, std::size_t searchFrom, float& out, std::size_t& valueEnd)
    {
        std::size_t valueStart = FindFieldValueStart(json, key, searchFrom);
        if (valueStart == NPos)
            return false;

        std::size_t end = ScanJsonNumberToken(json, valueStart);
        if (end == valueStart)
            return false;

        std::string token(json.substr(valueStart, end - valueStart));

        try
        {
            std::size_t consumed = 0;
            double value = std::stod(token, &consumed);
            if (consumed != token.size())
                return false;
            if (!std::isfinite(value))
                return false;
            out = static_cast<float>(value);
        }
        catch (std::exception const&)
        {
            return false;
        }

        valueEnd = end;
        return true;
    }

    // Locates the '{'...'}' object value of `key` at or after
    // `searchFrom`. String-literal aware: a quote, brace or bracket
    // inside a string value never affects depth tracking, so this works
    // correctly against real free text (unlike /decision's
    // FindObjectField() in AIClient.cpp, which assumes no string value
    // ever contains '{'/'}' - true for /decision's enum-only schema, not
    // for this one). Returns the span as [spanStart, spanEnd) with
    // spanStart pointing at '{' and spanEnd one past the matching '}'.
    bool FindJsonObjectSpan(std::string_view json, std::string_view key, std::size_t searchFrom, std::size_t& spanStart, std::size_t& spanEnd)
    {
        std::size_t valueStart = FindFieldValueStart(json, key, searchFrom);
        if (valueStart == NPos || valueStart >= json.size() || json[valueStart] != '{')
            return false;

        int depth = 0;
        std::size_t i = valueStart;
        for (; i < json.size(); ++i)
        {
            char c = json[i];
            if (c == '"')
            {
                std::size_t literalEnd = SkipJsonStringLiteral(json, i);
                if (literalEnd == NPos)
                    return false;
                i = literalEnd - 1; // the for-loop's ++i lands just past the closing quote
                continue;
            }
            if (c == '{')
                ++depth;
            else if (c == '}')
            {
                --depth;
                if (depth == 0)
                {
                    spanStart = valueStart;
                    spanEnd = i + 1;
                    return true;
                }
            }
        }
        return false;
    }

    bool ParseQuestObjectiveType(std::string const& text, QuestObjectiveType& out)
    {
        if (text == "KILL_CREATURE")
        {
            out = QuestObjectiveType::KillCreature;
            return true;
        }
        return false; // "INVALID" and anything unrecognized - reject, never guess
    }
}

std::string SerializeDynamicTaskRequest(DynamicTaskRequest const& request)
{
    std::ostringstream body;
    body << "{\"protocol_version\":" << ToUnderlying(request.Version)
         << ",\"request_id\":" << request.RequestId
         << ",\"context\":" << SerializeQuestContext(request.Context)
         << "}";
    return body.str();
}

bool ParseDynamicTaskResponse(std::string_view json, DynamicTaskResponse& response)
{
    constexpr uint64 Uint32Max = std::numeric_limits<uint32>::max();

    std::size_t cursor = 0;

    uint64 protocolVersion = 0;
    if (!FindJsonUInt64Field(json, "protocol_version", cursor, protocolVersion, cursor) || protocolVersion > Uint32Max)
        return false;

    uint64 requestId = 0;
    if (!FindJsonUInt64Field(json, "request_id", cursor, requestId, cursor))
        return false;

    uint64 agentId = 0;
    if (!FindJsonUInt64Field(json, "agent_id", cursor, agentId, cursor))
        return false;

    uint64 snapshotSequence = 0;
    if (!FindJsonUInt64Field(json, "snapshot_sequence", cursor, snapshotSequence, cursor))
        return false;

    std::size_t proposalStart = 0;
    std::size_t proposalEnd = 0;
    if (!FindJsonObjectSpan(json, "proposal", cursor, proposalStart, proposalEnd))
        return false;

    std::string_view proposalJson = json.substr(proposalStart, proposalEnd - proposalStart);
    std::size_t proposalCursor = 0;

    QuestProposalDraft draft;

    std::string objectiveText;
    if (!FindJsonStringField(proposalJson, "objective", proposalCursor, objectiveText, proposalCursor))
        return false;
    if (!ParseQuestObjectiveType(objectiveText, draft.Objective))
        return false;

    uint64 targetToken = 0;
    if (!FindJsonUInt64Field(proposalJson, "target_token", proposalCursor, targetToken, proposalCursor) || targetToken > Uint32Max)
        return false;
    draft.TargetToken = static_cast<uint32>(targetToken);

    uint64 requiredCount = 0;
    if (!FindJsonUInt64Field(proposalJson, "required_count", proposalCursor, requiredCount, proposalCursor))
        return false;
    if (requiredCount == 0 || requiredCount > Uint32Max)
        return false;
    draft.RequiredCount = static_cast<uint32>(requiredCount);

    float maxRangeYards = 0.0f;
    if (!FindJsonFloatField(proposalJson, "max_range_yards", proposalCursor, maxRangeYards, proposalCursor))
        return false;
    if (!(maxRangeYards > 0.0f))
        return false;
    draft.MaxRangeYards = maxRangeYards;

    uint64 expiryMs = 0;
    if (!FindJsonUInt64Field(proposalJson, "expiry_ms", proposalCursor, expiryMs, proposalCursor))
        return false;
    if (expiryMs == 0 || expiryMs > Uint32Max)
        return false;
    draft.ExpiryMs = static_cast<uint32>(expiryMs);

    uint64 rewardMoneyCopper = 0;
    if (!FindJsonUInt64Field(proposalJson, "reward_money_copper", proposalCursor, rewardMoneyCopper, proposalCursor) || rewardMoneyCopper > Uint32Max)
        return false;
    draft.RewardMoneyCopper = static_cast<uint32>(rewardMoneyCopper);

    std::string title;
    if (!FindJsonStringField(proposalJson, "title", proposalCursor, title, proposalCursor))
        return false;
    if (title.size() > QuestContractMaxTitleLength)
        return false;
    draft.Title = std::move(title);

    std::string description;
    if (!FindJsonStringField(proposalJson, "description", proposalCursor, description, proposalCursor))
        return false;
    if (description.size() > QuestContractMaxDescriptionLength)
        return false;
    draft.Description = std::move(description);

    response.Version = static_cast<DynamicTaskProtocolVersion>(static_cast<uint32>(protocolVersion));
    response.RequestId = requestId;
    response.Agent.Value = agentId;
    response.SnapshotSequence = snapshotSequence;
    response.Proposal = std::move(draft);

    return true;
}
