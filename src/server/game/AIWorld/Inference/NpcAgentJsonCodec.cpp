/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "NpcAgentJsonCodec.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>

namespace
{
    bool OneOf(std::string_view value, std::initializer_list<std::string_view> choices)
    {
        return std::find(choices.begin(), choices.end(), value) != choices.end();
    }

    bool KnownEmote(std::string_view value)
    {
        return OneOf(value, {"NONE", "WAVE", "TALK", "NOD", "LAUGH", "POINT", "CHEER", "NO"});
    }

    // Validate scalar UTF-8, excluding overlong forms, surrogate values and
    // values beyond Unicode. Plain chat also excludes controls and WoW markup.
    bool Text(std::string_view value, std::size_t maxBytes, std::size_t maxCharacters, bool plain = false, bool allowLines = false)
    {
        if (value.size() > maxBytes) return false;
        std::size_t count = 0;
        for (std::size_t i = 0; i < value.size(); )
        {
            unsigned char lead = static_cast<unsigned char>(value[i++]);
            uint32 cp = lead;
            unsigned length = 0;
            if (lead >= 0xC2 && lead <= 0xDF) { cp = lead & 0x1F; length = 1; }
            else if (lead >= 0xE0 && lead <= 0xEF) { cp = lead & 0x0F; length = 2; }
            else if (lead >= 0xF0 && lead <= 0xF4) { cp = lead & 0x07; length = 3; }
            else if (lead >= 0x80) return false;
            if (length > value.size() - i) return false;
            for (unsigned j = 0; j < length; ++j)
            {
                unsigned char next = static_cast<unsigned char>(value[i++]);
                if ((next & 0xC0) != 0x80) return false;
                cp = (cp << 6) | (next & 0x3F);
            }
            if ((length == 1 && cp < 0x80) || (length == 2 && cp < 0x800) ||
                (length == 3 && cp < 0x10000) || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
                return false;
            if (plain && !(allowLines && (cp == '\n' || cp == '\t')) &&
                (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == '|')) return false;
            if (++count > maxCharacters) return false;
        }
        return true;
    }

    void Quote(std::ostream& out, std::string const& value)
    {
        static char const hex[] = "0123456789abcdef";
        out << '"';
        for (unsigned char c : value)
            switch (c)
            {
                case '"': out << "\\\""; break;
                case '\\': out << "\\\\"; break;
                case '\n': out << "\\n"; break;
                case '\r': out << "\\r"; break;
                case '\t': out << "\\t"; break;
                default:
                    if (c < 0x20) out << "\\u00" << hex[c >> 4] << hex[c & 15];
                    else out << static_cast<char>(c);
                    break;
            }
        out << '"';
    }

    // This schema-only cursor follows the structured dynamic-task codec's
    // escape-aware, direct-object-member design. Each callback consumes the
    // declared value type; arrays, null, floats and nested surprises cannot
    // stand in for an integer or string. Every member is required exactly once.
    class JsonCursor
    {
    public:
        explicit JsonCursor(std::string_view json) : _json(json) { }
        bool End() { Space(); return _pos == _json.size(); }

        template<class Reader> bool Object(Reader read, std::size_t required)
        {
            if (!Take('{')) return false;
            std::vector<std::string> keys;
            if (Take('}')) return required == 0;
            for (;;)
            {
                std::string key;
                if (!String(key) || key.size() > 64 ||
                    std::find(keys.begin(), keys.end(), key) != keys.end() || !Take(':')) return false;
                keys.push_back(key);
                if (keys.size() > required || !read(key)) return false;
                if (Take('}')) return keys.size() == required;
                if (!Take(',')) return false;
            }
        }

        bool Uint(uint64& value)
        {
            Space();
            std::size_t start = _pos;
            uint64 candidate = 0;
            while (_pos < _json.size() && _json[_pos] >= '0' && _json[_pos] <= '9')
            {
                unsigned digit = static_cast<unsigned>(_json[_pos++] - '0');
                if (candidate > (std::numeric_limits<uint64>::max() - digit) / 10) return false;
                candidate = candidate * 10 + digit;
            }
            if (_pos == start || (_pos - start > 1 && _json[start] == '0')) return false;
            value = candidate;
            return true;
        }

        bool Uint32(uint32& value)
        {
            uint64 candidate = 0;
            if (!Uint(candidate) || candidate > std::numeric_limits<uint32>::max()) return false;
            value = static_cast<uint32>(candidate);
            return true;
        }

        bool String(std::string& value)
        {
            Space();
            if (_pos >= _json.size() || _json[_pos++] != '"') return false;
            std::string candidate;
            while (_pos < _json.size())
            {
                unsigned char c = static_cast<unsigned char>(_json[_pos++]);
                if (c == '"')
                {
                    if (!Text(candidate, NpcAgentResponseBodyLimit, NpcAgentResponseBodyLimit)) return false;
                    value = std::move(candidate);
                    return true;
                }
                if (c < 0x20) return false;
                if (c != '\\') { candidate.push_back(static_cast<char>(c)); continue; }
                if (_pos >= _json.size()) return false;
                switch (_json[_pos++])
                {
                    case '"': candidate.push_back('"'); break;
                    case '\\': candidate.push_back('\\'); break;
                    case '/': candidate.push_back('/'); break;
                    case 'b': candidate.push_back('\b'); break;
                    case 'f': candidate.push_back('\f'); break;
                    case 'n': candidate.push_back('\n'); break;
                    case 'r': candidate.push_back('\r'); break;
                    case 't': candidate.push_back('\t'); break;
                    case 'u':
                    {
                        uint32 cp = 0;
                        if (!Hex4(cp)) return false;
                        if (cp >= 0xD800 && cp <= 0xDBFF)
                        {
                            if (_json.size() - _pos < 6 || _json[_pos++] != '\\' || _json[_pos++] != 'u') return false;
                            uint32 low = 0;
                            if (!Hex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
                            cp = 0x10000 + ((cp - 0xD800) << 10) + low - 0xDC00;
                        }
                        else if (cp >= 0xDC00 && cp <= 0xDFFF) return false;
                        AppendCodePoint(candidate, cp);
                        break;
                    }
                    default: return false;
                }
            }
            return false;
        }

    private:
        void Space()
        {
            while (_pos < _json.size() && (_json[_pos] == ' ' || _json[_pos] == '\t' ||
                _json[_pos] == '\r' || _json[_pos] == '\n')) ++_pos;
        }
        bool Take(char c)
        {
            Space();
            if (_pos >= _json.size() || _json[_pos] != c) return false;
            ++_pos;
            return true;
        }
        bool Hex4(uint32& value)
        {
            if (_json.size() - _pos < 4) return false;
            value = 0;
            for (unsigned i = 0; i < 4; ++i)
            {
                unsigned char c = static_cast<unsigned char>(_json[_pos++]);
                unsigned digit;
                if (c >= '0' && c <= '9') digit = c - '0';
                else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
                else return false;
                value = (value << 4) | digit;
            }
            return true;
        }
        static void AppendCodePoint(std::string& out, uint32 cp)
        {
            if (cp <= 0x7F) out.push_back(static_cast<char>(cp));
            else if (cp <= 0x7FF)
            {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            else if (cp <= 0xFFFF)
            {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            else
            {
                out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }
        std::string_view _json;
        std::size_t _pos = 0;
    };

    template<class T> bool UniqueTokens(std::vector<T> const& values)
    {
        for (auto a = values.begin(); a != values.end(); ++a)
        {
            if (!a->Token) return false;
            for (auto b = values.begin(); b != a; ++b)
                if (a->Token == b->Token) return false;
        }
        return true;
    }

    bool ValidRequest(NpcAgentRequest const& request)
    {
        auto const& context = request.Context;
        auto const& self = context.Self;
        auto pressure = [](float p) { return std::isfinite(p) && p >= 0 && p <= 1; };
        if (request.ProtocolVersion != 1 || !request.RequestId || !request.Agent.Value ||
            !Text(context.Profile.Name, 80, 80, true) || context.Profile.Name.empty() ||
            !Text(context.Profile.Backstory, 8192, 8192, true, true) || !Text(context.Profile.Personality, 2048, 2048, true, true) ||
            !std::isfinite(self.X) || !std::isfinite(self.Y) || !std::isfinite(self.Z) || !std::isfinite(self.Orientation) ||
            !Text(self.CurrentAction, 128, 32, true) || !Text(self.LastActionResult, 256, 128, true) ||
            !pressure(context.Needs.HealthPressure) || !pressure(context.Needs.Hunger) ||
            !pressure(context.Needs.Fatigue) || !pressure(context.Needs.SafetyPressure) || !pressure(context.Needs.ResourcePressure) ||
            context.RecentDialogue.size() > 12 || context.Nearby.size() > 16 || context.Actions.size() > 32 ||
            context.Actions.empty() || context.Emotes.empty() || context.Emotes.size() > 8 || context.SpeechTargets.size() > 16 ||
            !UniqueTokens(context.Nearby) || !UniqueTokens(context.Actions)) return false;
        for (auto const& line : context.RecentDialogue)
            if (line.Speaker.empty() || !Text(line.Speaker, 80, 80, true) || !Text(line.Text, 768, 256, true) ||
                !OneOf(line.Kind, {"PLAYER", "NPC", "SELF"})) return false;
        for (auto const& entity : context.Nearby)
            if (entity.Name.empty() || !Text(entity.Name, 80, 80, true) || !OneOf(entity.Kind, {"PLAYER", "NPC"}) ||
                !std::isfinite(entity.Distance) || entity.Distance < 0) return false;
        for (auto const& action : context.Actions)
            if (!OneOf(action.Type, {"WAIT", "MOVE_TO", "EAT", "REST", "STOP", "FLEE"}) ||
                !Text(action.Description, 256, 256, true)) return false;
        for (auto it = context.Emotes.begin(); it != context.Emotes.end(); ++it)
            if (!KnownEmote(*it) || std::find(context.Emotes.begin(), it, *it) != it) return false;
        for (auto it = context.SpeechTargets.begin(); it != context.SpeechTargets.end(); ++it)
            if (!*it || std::find(context.SpeechTargets.begin(), it, *it) != it ||
                std::none_of(context.Nearby.begin(), context.Nearby.end(), [&](auto const& entity) { return entity.Token == *it; }))
                return false;
        return true;
    }
}

std::string SerializeNpcAgentRequest(NpcAgentRequest const& request)
{
    if (!ValidRequest(request)) return {};
    auto const& c = request.Context;
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<float>::max_digits10);
    out << "{\"protocol_version\":" << request.ProtocolVersion << ",\"request_id\":" << request.RequestId
        << ",\"agent_id\":" << request.Agent.Value << ",\"snapshot_sequence\":" << request.SnapshotSequence
        << ",\"context\":{\"profile\":{\"name\":";
    Quote(out, c.Profile.Name); out << ",\"backstory\":"; Quote(out, c.Profile.Backstory);
    out << ",\"personality\":"; Quote(out, c.Profile.Personality);
    out << "},\"needs\":{\"health_pressure\":" << c.Needs.HealthPressure << ",\"hunger\":" << c.Needs.Hunger
        << ",\"fatigue\":" << c.Needs.Fatigue << ",\"safety_pressure\":" << c.Needs.SafetyPressure
        << ",\"resource_pressure\":" << c.Needs.ResourcePressure << "},\"self\":{\"map_id\":" << c.Self.MapId
        << ",\"position\":{\"x\":" << c.Self.X << ",\"y\":" << c.Self.Y << ",\"z\":" << c.Self.Z
        << ",\"orientation\":" << c.Self.Orientation << "},\"health\":" << c.Self.Health
        << ",\"max_health\":" << c.Self.MaxHealth << ",\"alive\":" << (c.Self.Alive ? "true" : "false")
        << ",\"in_combat\":" << (c.Self.InCombat ? "true" : "false") << ",\"current_action\":";
    Quote(out, c.Self.CurrentAction); out << ",\"last_action_result\":"; Quote(out, c.Self.LastActionResult);
    out << "},\"recent_dialogue\":[";
    for (std::size_t i = 0; i < c.RecentDialogue.size(); ++i)
    {
        if (i) out << ',';
        out << "{\"speaker\":"; Quote(out, c.RecentDialogue[i].Speaker);
        out << ",\"text\":"; Quote(out, c.RecentDialogue[i].Text);
        out << ",\"kind\":"; Quote(out, c.RecentDialogue[i].Kind); out << '}';
    }
    out << "],\"nearby\":[";
    for (std::size_t i = 0; i < c.Nearby.size(); ++i)
    {
        if (i) out << ',';
        out << "{\"token\":" << c.Nearby[i].Token << ",\"name\":"; Quote(out, c.Nearby[i].Name);
        out << ",\"kind\":"; Quote(out, c.Nearby[i].Kind); out << ",\"distance\":" << c.Nearby[i].Distance << '}';
    }
    out << "],\"actions\":[";
    for (std::size_t i = 0; i < c.Actions.size(); ++i)
    {
        if (i) out << ',';
        out << "{\"token\":" << c.Actions[i].Token << ",\"type\":"; Quote(out, c.Actions[i].Type);
        out << ",\"description\":"; Quote(out, c.Actions[i].Description); out << '}';
    }
    out << "],\"emotes\":[";
    for (std::size_t i = 0; i < c.Emotes.size(); ++i) { if (i) out << ','; Quote(out, c.Emotes[i]); }
    out << "],\"speech_targets\":[";
    for (std::size_t i = 0; i < c.SpeechTargets.size(); ++i) { if (i) out << ','; out << c.SpeechTargets[i]; }
    out << "]}}";
    auto result = out.str();
    return result.size() <= NpcAgentRequestBodyLimit ? result : std::string();
}

bool ParseNpcAgentResponse(std::string_view json, NpcAgentResponse& response)
{
    if (json.size() > NpcAgentResponseBodyLimit) return false;
    NpcAgentResponse candidate;
    JsonCursor cursor(json);
    if (!cursor.Object([&](std::string const& key)
    {
        if (key == "protocol_version") return cursor.Uint32(candidate.ProtocolVersion);
        if (key == "request_id") return cursor.Uint(candidate.RequestId);
        if (key == "agent_id") return cursor.Uint(candidate.Agent.Value);
        if (key == "snapshot_sequence") return cursor.Uint(candidate.SnapshotSequence);
        if (key == "decision") return cursor.Object([&](std::string const& field)
        {
            if (field == "action_token") return cursor.Uint32(candidate.Decision.ActionToken);
            if (field == "speech") return cursor.String(candidate.Decision.Speech);
            if (field == "speech_target") return cursor.Uint32(candidate.Decision.SpeechTarget);
            if (field == "emote") return cursor.String(candidate.Decision.Emote);
            return false;
        }, 4);
        return false;
    }, 5) || !cursor.End() || candidate.ProtocolVersion != 1 || !candidate.RequestId ||
        !candidate.Agent.Value || !KnownEmote(candidate.Decision.Emote) ||
        !Text(candidate.Decision.Speech, 768, 256, true)) return false;
    response = std::move(candidate);
    return true;
}

bool MatchesNpcAgentResponse(NpcAgentRequest const& request, NpcAgentResponse const& response)
{
    auto const& decision = response.Decision;
    if (response.ProtocolVersion != request.ProtocolVersion || response.RequestId != request.RequestId ||
        response.Agent != request.Agent || response.SnapshotSequence != request.SnapshotSequence ||
        !Text(decision.Speech, 768, 256, true) || !KnownEmote(decision.Emote)) return false;
    if (decision.ActionToken && std::none_of(request.Context.Actions.begin(), request.Context.Actions.end(),
        [&](auto const& action) { return action.Token == decision.ActionToken; })) return false;
    if (decision.SpeechTarget &&
        std::find(request.Context.SpeechTargets.begin(), request.Context.SpeechTargets.end(), decision.SpeechTarget) ==
            request.Context.SpeechTargets.end()) return false;
    return std::find(request.Context.Emotes.begin(), request.Context.Emotes.end(), decision.Emote) != request.Context.Emotes.end();
}
