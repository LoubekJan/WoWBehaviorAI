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
    // ---------------------------------------------------------------

    constexpr std::size_t NPos = std::string_view::npos;

    // Returns the index just past the closing, unescaped '"' of the
    // string literal starting at json[start] (which must itself be '"'),
    // or NPos if the literal is unterminated, ends mid-escape, contains
    // an illegal escape sequence, or contains a raw (unescaped) control
    // character - all of which are illegal inside a JSON string per
    // RFC 8259, not just "inconvenient to skip over". Legal escapes are
    // \" \\ \/ \b \f \n \r \t and \uXXXX (four hex digits) - anything
    // else is a hard failure, the same set UnescapeJsonString below
    // actually decodes. Used both by the grammar validator (SkipJsonValue)
    // and by the schema-specific field extractors further down, so a
    // string this function has already walked is *known* well-formed by
    // the time either UnescapeJsonString or a brace-depth scan looks at
    // it - not just "probably fine".
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

    // -----------------------------------------------------------------
    // Full JSON grammar validator (RFC 8259) - review follow-up.
    //
    // The Find*Field()/FindJsonObjectSpan() helpers above locate this
    // schema's known fields by searching for "key": patterns; on their
    // own they never confirm the *document itself* is legal JSON - a
    // body missing its root braces, commas, or containing trailing
    // garbage after the real object could still have every expected key
    // "found" and accepted. ParseDynamicTaskResponse() closes that gap by
    // running this validator over the *entire* body first: it recursively
    // walks the standard JSON grammar (object/array/string/number/true/
    // false/null), rejects duplicate keys within any one object, and
    // requires the whole input to be consumed by exactly one root object
    // - only once all of that holds does schema-specific extraction ever
    // run. This does not build a DOM (nothing here is kept - callers get
    // pass/fail only) and does not reject unrecognized/extra keys, only
    // malformed ones - the philosophy /decision's own parser already
    // documents (tolerant of unknown extra keys, never of a broken shape).
    std::size_t SkipJsonWhitespace(std::string_view json, std::size_t pos)
    {
        while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])))
            ++pos;
        return pos;
    }

    // Strict RFC 8259 number grammar: -?(0|[1-9]\d*)(\.\d+)?([eE][+-]?\d+)?
    // - unlike ScanJsonNumberToken above (deliberately lenient, used only
    // to find a token's raw span for std::stoull/std::stod to judge),
    // this rejects e.g. a leading zero ("007"), a bare "-", or a "."/'e'
    // with no digits after it. Returns `start` if no legal number begins
    // there.
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

    std::size_t SkipJsonValue(std::string_view json, std::size_t pos, std::size_t depth);

    // Validates '{'...'}' starting at json[start] (which must be '{'):
    // an empty object, or a comma-separated "key":value list, each value
    // itself grammar-validated recursively. Rejects a duplicate key
    // within this object (decoded, so A and "A" count as the same
    // key) - RFC 8259 permits it syntactically but this codec never
    // needs the ambiguity of "which one wins", so it simply refuses to
    // guess. Returns the index just past the matching '}', or NPos.
    std::size_t SkipJsonObject(std::string_view json, std::size_t start, std::size_t depth)
    {
        std::size_t i = SkipJsonWhitespace(json, start + 1);

        if (i < json.size() && json[i] == '}')
            return i + 1;

        std::vector<std::string> seenKeys; // this schema's objects have a handful of fields - linear scan is fine

        for (;;)
        {
            if (i >= json.size() || json[i] != '"')
                return NPos;

            std::size_t keyLiteralEnd = SkipJsonStringLiteral(json, i);
            if (keyLiteralEnd == NPos)
                return NPos;

            std::string key;
            if (!UnescapeJsonString(json.substr(i, keyLiteralEnd - i), key))
                return NPos;

            for (std::string const& seen : seenKeys)
                if (seen == key)
                    return NPos; // duplicate key

            seenKeys.push_back(std::move(key));

            i = SkipJsonWhitespace(json, keyLiteralEnd);
            if (i >= json.size() || json[i] != ':')
                return NPos;

            i = SkipJsonWhitespace(json, i + 1);

            i = SkipJsonValue(json, i, depth + 1);
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
            if (json[i] == '}')
                return i + 1;

            return NPos;
        }
    }

    // Validates '['...']' starting at json[start] (which must be '['):
    // an empty array, or a comma-separated value list. Returns the index
    // just past the matching ']', or NPos.
    std::size_t SkipJsonArray(std::string_view json, std::size_t start, std::size_t depth)
    {
        std::size_t i = SkipJsonWhitespace(json, start + 1);

        if (i < json.size() && json[i] == ']')
            return i + 1;

        for (;;)
        {
            i = SkipJsonValue(json, i, depth + 1);
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
    // skipped by the caller - object/array entries and the top-level
    // caller both do this before calling in). Returns the index just
    // past the value, or NPos if nothing legal starts there.
    std::size_t SkipJsonValue(std::string_view json, std::size_t pos, std::size_t depth)
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
            return SkipJsonObject(json, pos, depth);
        if (c == '[')
            return SkipJsonArray(json, pos, depth);
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

    // Validates that `json` is, in its entirety, exactly one legal JSON
    // object (leading/trailing whitespace aside) - not an array, string,
    // number or bare literal at the root, and nothing but whitespace
    // before or after it. See the block comment above SkipJsonWhitespace
    // for why ParseDynamicTaskResponse() runs this before touching any
    // Find*Field() helper.
    bool ValidateJsonRootObject(std::string_view json)
    {
        std::size_t rootPos = SkipJsonWhitespace(json, 0);
        if (rootPos >= json.size() || json[rootPos] != '{')
            return false;

        std::size_t rootEnd = SkipJsonObject(json, rootPos, 0);
        if (rootEnd == NPos)
            return false;

        return SkipJsonWhitespace(json, rootEnd) == json.size();
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
    // Grammar first, schema second (review follow-up): a body missing
    // its root braces/commas, or carrying trailing garbage after an
    // otherwise-valid object, must never reach the Find*Field() helpers
    // below - they locate known keys by substring search and do not, on
    // their own, prove the surrounding document is legal JSON at all.
    if (!ValidateJsonRootObject(json))
        return false;

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
