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
#include <initializer_list>
#include <limits>
#include <sstream>
#include <vector>

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
    //
    // Two layers, in order:
    //
    //  1. A generic recursive-descent JSON grammar walker
    //     (ParseJsonValue/ParseJsonObject/ParseJsonArray) that proves the
    //     body is legal JSON per RFC 8259 - matching braces/brackets,
    //     legal comma/colon placement, escape-aware strings, strict
    //     number grammar, no duplicate keys within any one object, and
    //     (at the root, via ParseJsonRootObjectMembers) full input
    //     consumption. This layer also *records* each object's direct
    //     members (decoded key -> raw value span) as it walks, rather
    //     than just returning pass/fail, so schema extraction below never
    //     has to search the document again.
    //
    //  2. Schema extraction (ParseDynamicTaskResponse) that looks up this
    //     schema's fields by name in the *already-parsed member list* of
    //     the exact object they belong to - never by substring search
    //     across the whole document. A field is only ever read from the
    //     direct members of the object it is declared on: this schema is
    //     "root: {protocol_version, request_id, agent_id,
    //     snapshot_sequence, proposal}" and "proposal:
    //     {objective, target_token, required_count, max_range_yards,
    //     expiry_ms, reward_money_copper, title, description}", and
    //     HasExactKeySet() below requires the member set at each level to
    //     match exactly - no missing key, no unknown/extra key, no
    //     duplicate (already impossible after layer 1) - the same
    //     "extra=forbid, all fields required" contract ai-server's own
    //     pydantic models enforce.
    // ---------------------------------------------------------------

    constexpr std::size_t NPos = std::string_view::npos;

    // JSON defines insignificant whitespace as exactly these four bytes
    // (RFC 8259 section 2) - deliberately not std::isspace(), which is
    // locale-dependent and, even in the "C" locale, additionally accepts
    // vertical tab (0x0B) and form feed (0x0C), neither of which JSON
    // permits between tokens.
    bool IsJsonWhitespace(char c)
    {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    }

    std::size_t SkipJsonWhitespace(std::string_view json, std::size_t pos)
    {
        while (pos < json.size() && IsJsonWhitespace(json[pos]))
            ++pos;
        return pos;
    }

    // Returns the index just past the closing, unescaped '"' of the
    // string literal starting at json[start] (which must itself be '"'),
    // or NPos if the literal is unterminated, ends mid-escape, contains
    // an illegal escape sequence, or contains a raw (unescaped) control
    // character - all of which are illegal inside a JSON string per
    // RFC 8259, not just "inconvenient to skip over". Legal escapes are
    // \" \\ \/ \b \f \n \r \t and \uXXXX (four hex digits) - anything
    // else is a hard failure, the same set UnescapeJsonString below
    // actually decodes.
    std::size_t SkipJsonStringLiteral(std::string_view json, std::size_t start)
    {
        if (start >= json.size() || json[start] != '"')
            return NPos;

        std::size_t i = start + 1;
        while (i < json.size())
        {
            unsigned char c = static_cast<unsigned char>(json[i]);
            if (c == '"')
                return i + 1;

            if (c == '\\')
            {
                if (i + 1 >= json.size())
                    return NPos;

                char esc = json[i + 1];
                switch (esc)
                {
                    case '"': case '\\': case '/': case 'b': case 'f': case 'n': case 'r': case 't':
                        i += 2;
                        break;
                    case 'u':
                        if (i + 6 > json.size())
                            return NPos;
                        for (int k = 0; k < 4; ++k)
                        {
                            char h = json[i + 2 + k];
                            bool hex = (h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F');
                            if (!hex)
                                return NPos;
                        }
                        i += 6;
                        break;
                    default:
                        return NPos; // unknown escape - reject, never guess
                }
                continue;
            }

            if (c < 0x20)
                return NPos; // raw control character - must be escaped, RFC 8259 section 7

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

    // Strict RFC 8259 number grammar: -?(0|[1-9]\d*)(\.\d+)?([eE][+-]?\d+)?
    // - rejects e.g. a leading zero ("007"), a bare "-", or a "."/'e' with
    // no digits after it. Returns `start` if no legal number begins there.
    std::size_t SkipStrictJsonNumber(std::string_view json, std::size_t start)
    {
        std::size_t i = start;
        if (i < json.size() && json[i] == '-')
            ++i;

        if (i >= json.size() || !std::isdigit(static_cast<unsigned char>(json[i])))
            return start;

        if (json[i] == '0')
            ++i; // a leading zero must stand alone in the integer part
        else
        {
            while (i < json.size() && std::isdigit(static_cast<unsigned char>(json[i])))
                ++i;
        }

        if (i < json.size() && json[i] == '.')
        {
            std::size_t j = i + 1;
            if (j >= json.size() || !std::isdigit(static_cast<unsigned char>(json[j])))
                return start; // '.' must be followed by at least one digit
            while (j < json.size() && std::isdigit(static_cast<unsigned char>(json[j])))
                ++j;
            i = j;
        }

        if (i < json.size() && (json[i] == 'e' || json[i] == 'E'))
        {
            std::size_t j = i + 1;
            if (j < json.size() && (json[j] == '+' || json[j] == '-'))
                ++j;
            if (j >= json.size() || !std::isdigit(static_cast<unsigned char>(json[j])))
                return start; // exponent marker must be followed by at least one digit
            while (j < json.size() && std::isdigit(static_cast<unsigned char>(json[j])))
                ++j;
            i = j;
        }

        return i;
    }

    // One direct member of a JSON object, as recorded by ParseJsonObject:
    // its decoded key, and the [ValueStart, ValueEnd) span of its value's
    // raw (still-JSON-encoded) text in the original document.
    struct JsonObjectMember
    {
        std::string Key;
        std::size_t ValueStart = 0;
        std::size_t ValueEnd = 0;
    };

    std::size_t ParseJsonValue(std::string_view json, std::size_t pos, std::size_t depth);

    // Validates '{'...'}' starting at json[start] (which must be '{'):
    // an empty object, or a comma-separated "key":value list, each value
    // itself grammar-validated recursively. Rejects a duplicate key
    // within this object (decoded, so an escaped and unescaped spelling
    // of the same key both count) - RFC 8259 permits it syntactically but
    // this codec never needs the ambiguity of "which one wins", so it
    // simply refuses to guess. When `membersOut` is non-null, every
    // direct (not recursively-nested) member is recorded into it in
    // encounter order; pass nullptr when only grammar validation matters
    // (e.g. skipping past a value neither ParseDynamicTaskResponse nor
    // this schema otherwise cares about). Returns the index just past the
    // matching '}', or NPos.
    std::size_t ParseJsonObject(std::string_view json, std::size_t start, std::size_t depth, std::vector<JsonObjectMember>* membersOut)
    {
        if (membersOut)
            membersOut->clear();

        std::size_t i = SkipJsonWhitespace(json, start + 1);

        if (i < json.size() && json[i] == '}')
            return i + 1;

        std::vector<std::string> seenKeys; // this schema's objects have a handful of fields - linear scan is fine

        for (;;)
        {
            if (i >= json.size() || json[i] != '"')
                return NPos;

            std::size_t keyStart = i;
            std::size_t keyLiteralEnd = SkipJsonStringLiteral(json, i);
            if (keyLiteralEnd == NPos)
                return NPos;

            std::string key;
            if (!UnescapeJsonString(json.substr(keyStart, keyLiteralEnd - keyStart), key))
                return NPos;

            for (std::string const& seen : seenKeys)
                if (seen == key)
                    return NPos; // duplicate key

            seenKeys.push_back(key);

            i = SkipJsonWhitespace(json, keyLiteralEnd);
            if (i >= json.size() || json[i] != ':')
                return NPos;

            i = SkipJsonWhitespace(json, i + 1);

            std::size_t valueStart = i;
            i = ParseJsonValue(json, i, depth + 1);
            if (i == NPos)
                return NPos;

            if (membersOut)
                membersOut->push_back(JsonObjectMember{ std::move(key), valueStart, i });

            i = SkipJsonWhitespace(json, i);
            if (i >= json.size())
                return NPos;

            if (json[i] == ',')
            {
                i = SkipJsonWhitespace(json, i + 1);
                continue;
            }
            if (json[i] == '}')
                return i + 1;

            return NPos;
        }
    }

    // Validates '['...']' starting at json[start] (which must be '['):
    // an empty array, or a comma-separated value list. This schema never
    // uses an array itself, but the grammar walker still has to be able
    // to validate (and skip past) one if it appears as some other field's
    // value. Returns the index just past the matching ']', or NPos.
    std::size_t ParseJsonArray(std::string_view json, std::size_t start, std::size_t depth)
    {
        std::size_t i = SkipJsonWhitespace(json, start + 1);

        if (i < json.size() && json[i] == ']')
            return i + 1;

        for (;;)
        {
            i = ParseJsonValue(json, i, depth + 1);
            if (i == NPos)
                return NPos;

            i = SkipJsonWhitespace(json, i);
            if (i >= json.size())
                return NPos;

            if (json[i] == ',')
            {
                i = SkipJsonWhitespace(json, i + 1);
                continue;
            }
            if (json[i] == ']')
                return i + 1;

            return NPos;
        }
    }

    // Validates exactly one JSON value (object/array/string/number/true/
    // false/null) starting at json[pos] (leading whitespace already
    // skipped by the caller). A nested object's own members are never
    // recorded here (ParseJsonObject is called with membersOut=nullptr) -
    // callers that need a *specific* nested object's members (i.e.
    // ParseDynamicTaskResponse, for "proposal") re-invoke ParseJsonObject
    // directly on that member's already-known-valid span. Returns the
    // index just past the value, or NPos if nothing legal starts there.
    std::size_t ParseJsonValue(std::string_view json, std::size_t pos, std::size_t depth)
    {
        // Milestone 2.13A3: bounds recursion against a maliciously deep
        // document (stack-overflow DoS) - this schema's own real nesting
        // is shallow (response -> proposal, 2 levels), so a generous but
        // finite bound is pure headroom, not a real constraint on valid
        // input.
        constexpr std::size_t MaxDepth = 32;
        if (depth > MaxDepth || pos >= json.size())
            return NPos;

        char c = json[pos];
        if (c == '{')
            return ParseJsonObject(json, pos, depth, nullptr);
        if (c == '[')
            return ParseJsonArray(json, pos, depth);
        if (c == '"')
            return SkipJsonStringLiteral(json, pos);
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c)))
        {
            std::size_t end = SkipStrictJsonNumber(json, pos);
            return end == pos ? NPos : end;
        }
        if (json.substr(pos, 4) == "true")
            return pos + 4;
        if (json.substr(pos, 5) == "false")
            return pos + 5;
        if (json.substr(pos, 4) == "null")
            return pos + 4;

        return NPos;
    }

    // Top-level entry point: `json` must be, in its entirety, exactly one
    // legal JSON object (leading/trailing whitespace aside) - not an
    // array, string, number or bare literal at the root, and nothing but
    // whitespace before or after it. On success, `members` holds the
    // root object's direct members; ParseDynamicTaskResponse() looks up
    // this schema's root-level fields there, never by searching `json`
    // itself again.
    bool ParseJsonRootObjectMembers(std::string_view json, std::vector<JsonObjectMember>& members)
    {
        std::size_t rootPos = SkipJsonWhitespace(json, 0);
        if (rootPos >= json.size() || json[rootPos] != '{')
            return false;

        std::size_t rootEnd = ParseJsonObject(json, rootPos, 0, &members);
        if (rootEnd == NPos)
            return false;

        return SkipJsonWhitespace(json, rootEnd) == json.size();
    }

    JsonObjectMember const* FindMember(std::vector<JsonObjectMember> const& members, std::string_view key)
    {
        for (JsonObjectMember const& member : members)
            if (member.Key == key)
                return &member;
        return nullptr;
    }

    // True only if `members` contains exactly `requiredKeys` - every
    // required key present, and no other key besides them. Members are
    // already known duplicate-free (ParseJsonObject rejects those), so
    // "same count as requiredKeys, and every required key found" is
    // sufficient to also rule out an extra/unknown member: fitting
    // |requiredKeys| distinct required keys plus any extra one would need
    // more than |requiredKeys| members, contradicting the count check.
    bool HasExactKeySet(std::vector<JsonObjectMember> const& members, std::initializer_list<std::string_view> requiredKeys)
    {
        if (members.size() != requiredKeys.size())
            return false;

        for (std::string_view required : requiredKeys)
            if (!FindMember(members, required))
                return false;

        return true;
    }

    // Interprets the JSON value spanning json[valueStart, valueEnd) as a
    // non-negative integer literal (wrong JSON type - e.g. a string,
    // object, or a negative/fractional/exponent number - is rejected,
    // never coerced). The span is assumed already grammar-validated by
    // ParseJsonObject/ParseJsonValue.
    bool ParseUInt64Value(std::string_view json, std::size_t valueStart, std::size_t valueEnd, uint64& out)
    {
        if (valueEnd <= valueStart)
            return false;

        std::string token(json.substr(valueStart, valueEnd - valueStart));
        for (char c : token)
            if (!std::isdigit(static_cast<unsigned char>(c)))
                return false; // not a plain digit-only literal - wrong type or wrong shape

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

        return true;
    }

    // Interprets the JSON value spanning json[valueStart, valueEnd) as a
    // finite float, rejecting a value that would overflow float's finite
    // range when narrowed from double (see the block comment on the
    // narrowing check below) as well as any non-numeric JSON type.
    bool ParseFloatValue(std::string_view json, std::size_t valueStart, std::size_t valueEnd, float& out)
    {
        if (valueEnd <= valueStart)
            return false;

        std::string token(json.substr(valueStart, valueEnd - valueStart));
        if (!(token[0] == '-' || std::isdigit(static_cast<unsigned char>(token[0]))))
            return false; // first byte of a validated JSON number is always '-' or a digit - anything else is a different JSON type here (e.g. a quoted string)

        try
        {
            std::size_t consumed = 0;
            double value = std::stod(token, &consumed);
            if (consumed != token.size() || !std::isfinite(value))
                return false;

            // A finite double can still be outside float's finite range
            // (e.g. 1e100) - narrowing that via static_cast is undefined
            // behavior in general and, on the implementations where it
            // isn't, typically yields +-inf. Reject before narrowing
            // rather than trust an isfinite() check already made obsolete
            // by the cast that follows it.
            if (value < -static_cast<double>(std::numeric_limits<float>::max()) ||
                value > static_cast<double>(std::numeric_limits<float>::max()))
                return false;

            float narrowed = static_cast<float>(value);
            if (!std::isfinite(narrowed))
                return false;

            out = narrowed;
        }
        catch (std::exception const&)
        {
            return false;
        }

        return true;
    }

    // Interprets the JSON value spanning json[valueStart, valueEnd) as a
    // string, rejecting any other JSON type (a bare number/object/etc
    // there is a schema violation, not "close enough").
    bool ParseStringValue(std::string_view json, std::size_t valueStart, std::size_t valueEnd, std::string& out)
    {
        if (valueEnd - valueStart < 2 || json[valueStart] != '"')
            return false;

        return UnescapeJsonString(json.substr(valueStart, valueEnd - valueStart), out);
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
    std::vector<JsonObjectMember> rootMembers;
    if (!ParseJsonRootObjectMembers(json, rootMembers))
        return false;

    // Root must be exactly {protocol_version, request_id, agent_id,
    // snapshot_sequence, proposal} - no missing field, no extra/unknown
    // one (mirrors ai-server's own extra="forbid" pydantic models; see
    // DynamicTaskResponse in docker/ai/app/dynamic_task.py). Every
    // FindMember() below is therefore guaranteed non-null - each of
    // these five keys was individually confirmed present by
    // HasExactKeySet() already.
    if (!HasExactKeySet(rootMembers, { "protocol_version", "request_id", "agent_id", "snapshot_sequence", "proposal" }))
        return false;

    JsonObjectMember const* protocolVersionMember = FindMember(rootMembers, "protocol_version");
    JsonObjectMember const* requestIdMember = FindMember(rootMembers, "request_id");
    JsonObjectMember const* agentIdMember = FindMember(rootMembers, "agent_id");
    JsonObjectMember const* snapshotSequenceMember = FindMember(rootMembers, "snapshot_sequence");
    JsonObjectMember const* proposalMember = FindMember(rootMembers, "proposal");

    constexpr uint64 Uint32Max = std::numeric_limits<uint32>::max();

    uint64 protocolVersion = 0;
    if (!ParseUInt64Value(json, protocolVersionMember->ValueStart, protocolVersionMember->ValueEnd, protocolVersion) || protocolVersion > Uint32Max)
        return false;

    uint64 requestId = 0;
    if (!ParseUInt64Value(json, requestIdMember->ValueStart, requestIdMember->ValueEnd, requestId))
        return false;

    uint64 agentId = 0;
    if (!ParseUInt64Value(json, agentIdMember->ValueStart, agentIdMember->ValueEnd, agentId))
        return false;

    uint64 snapshotSequence = 0;
    if (!ParseUInt64Value(json, snapshotSequenceMember->ValueStart, snapshotSequenceMember->ValueEnd, snapshotSequence))
        return false;

    // "proposal" must itself be a direct-member object, not any other
    // JSON type - re-running ParseJsonObject on its own already-validated
    // span both confirms that and collects its own direct members in one
    // step (this repeats grammar work ParseJsonRootObjectMembers already
    // did for this exact span, but that's cheap for an object this small,
    // and keeps this function from having to carry a full parsed-value
    // tree just for one nested level).
    if (proposalMember->ValueEnd <= proposalMember->ValueStart || json[proposalMember->ValueStart] != '{')
        return false;

    std::vector<JsonObjectMember> proposalMembers;
    if (ParseJsonObject(json, proposalMember->ValueStart, 1, &proposalMembers) != proposalMember->ValueEnd)
        return false;

    // Same exact-set discipline as the root, for "proposal"'s own
    // {objective, target_token, required_count, max_range_yards,
    // expiry_ms, reward_money_copper, title, description} - see
    // QuestProposalDraft in docker/ai/app/dynamic_task.py.
    if (!HasExactKeySet(proposalMembers, { "objective", "target_token", "required_count", "max_range_yards",
        "expiry_ms", "reward_money_copper", "title", "description" }))
        return false;

    JsonObjectMember const* objectiveMember = FindMember(proposalMembers, "objective");
    JsonObjectMember const* targetTokenMember = FindMember(proposalMembers, "target_token");
    JsonObjectMember const* requiredCountMember = FindMember(proposalMembers, "required_count");
    JsonObjectMember const* maxRangeYardsMember = FindMember(proposalMembers, "max_range_yards");
    JsonObjectMember const* expiryMsMember = FindMember(proposalMembers, "expiry_ms");
    JsonObjectMember const* rewardMoneyCopperMember = FindMember(proposalMembers, "reward_money_copper");
    JsonObjectMember const* titleMember = FindMember(proposalMembers, "title");
    JsonObjectMember const* descriptionMember = FindMember(proposalMembers, "description");

    QuestProposalDraft draft;

    std::string objectiveText;
    if (!ParseStringValue(json, objectiveMember->ValueStart, objectiveMember->ValueEnd, objectiveText))
        return false;
    if (!ParseQuestObjectiveType(objectiveText, draft.Objective))
        return false;

    uint64 targetToken = 0;
    if (!ParseUInt64Value(json, targetTokenMember->ValueStart, targetTokenMember->ValueEnd, targetToken) || targetToken > Uint32Max)
        return false;
    draft.TargetToken = static_cast<uint32>(targetToken);

    uint64 requiredCount = 0;
    if (!ParseUInt64Value(json, requiredCountMember->ValueStart, requiredCountMember->ValueEnd, requiredCount))
        return false;
    if (requiredCount == 0 || requiredCount > Uint32Max)
        return false;
    draft.RequiredCount = static_cast<uint32>(requiredCount);

    float maxRangeYards = 0.0f;
    if (!ParseFloatValue(json, maxRangeYardsMember->ValueStart, maxRangeYardsMember->ValueEnd, maxRangeYards))
        return false;
    if (!(maxRangeYards > 0.0f))
        return false;
    draft.MaxRangeYards = maxRangeYards;

    uint64 expiryMs = 0;
    if (!ParseUInt64Value(json, expiryMsMember->ValueStart, expiryMsMember->ValueEnd, expiryMs))
        return false;
    if (expiryMs == 0 || expiryMs > Uint32Max)
        return false;
    draft.ExpiryMs = static_cast<uint32>(expiryMs);

    uint64 rewardMoneyCopper = 0;
    if (!ParseUInt64Value(json, rewardMoneyCopperMember->ValueStart, rewardMoneyCopperMember->ValueEnd, rewardMoneyCopper) || rewardMoneyCopper > Uint32Max)
        return false;
    draft.RewardMoneyCopper = static_cast<uint32>(rewardMoneyCopper);

    std::string title;
    if (!ParseStringValue(json, titleMember->ValueStart, titleMember->ValueEnd, title))
        return false;
    if (title.size() > QuestContractMaxTitleLength)
        return false;
    draft.Title = std::move(title);

    std::string description;
    if (!ParseStringValue(json, descriptionMember->ValueStart, descriptionMember->ValueEnd, description))
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
