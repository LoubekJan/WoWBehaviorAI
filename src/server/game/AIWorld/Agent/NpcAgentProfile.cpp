/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "NpcAgentProfile.h"
#include "NpcAgentDialogue.h"
#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace
{
    class ProfileParser
    {
    public:
        explicit ProfileParser(std::string_view json) : _json(json) { }

        bool Parse(NpcAgentProfile& result)
        {
            if (!Take('{'))
                return false;
            NpcAgentProfile parsed;
            std::unordered_set<std::string> keys;
            for (std::size_t field = 0; field < 6; ++field)
            {
                if (field && !Take(','))
                    return false;
                std::string key;
                if (!String(key) || !keys.insert(key).second || !Take(':'))
                    return false;
                if (key == "agent_id")
                {
                    if (!Unsigned(parsed.Agent.Value)) return false;
                }
                else if (key == "spawn_id")
                {
                    if (!Unsigned(parsed.SpawnId)) return false;
                }
                else if (key == "map_id")
                {
                    uint64 mapId = 0;
                    if (!Unsigned(mapId) || mapId > std::numeric_limits<uint32>::max()) return false;
                    parsed.MapId = uint32(mapId);
                }
                else if (key == "name")
                {
                    if (!String(parsed.Name)) return false;
                }
                else if (key == "backstory")
                {
                    if (!String(parsed.Backstory)) return false;
                }
                else if (key == "personality")
                {
                    if (!String(parsed.Personality)) return false;
                }
                else
                    return false;
            }
            if (!Take('}'))
                return false;
            NormalizeAuthoredLines(parsed.Backstory);
            NormalizeAuthoredLines(parsed.Personality);
            Whitespace();
            if (_position != _json.size() || !parsed.Agent || !parsed.SpawnId || parsed.Agent.Value != parsed.SpawnId
                || !ValidateNpcAgentPlainText(parsed.Name, NpcAgentDialogueLimits::MaxNameBytes,
                    NpcAgentDialogueLimits::MaxNameCharacters)
                || !ValidateNpcAgentPlainText(parsed.Backstory, 8192, 8192, true)
                || !ValidateNpcAgentPlainText(parsed.Personality, 2048, 2048, true))
                return false;
            result = std::move(parsed);
            return true;
        }

    private:
        static void NormalizeAuthoredLines(std::string& text)
        {
            std::string normalized;
            normalized.reserve(text.size());
            for (std::size_t position = 0; position < text.size(); ++position)
            {
                if (text[position] == '\r' && position + 1 < text.size() && text[position + 1] == '\n')
                    continue;
                normalized.push_back(text[position]);
            }
            text = std::move(normalized);
        }

        void Whitespace()
        {
            while (_position < _json.size() && (_json[_position] == ' ' || _json[_position] == '\t'
                || _json[_position] == '\r' || _json[_position] == '\n'))
                ++_position;
        }

        bool Take(char expected)
        {
            Whitespace();
            if (_position >= _json.size() || _json[_position] != expected)
                return false;
            ++_position;
            return true;
        }

        bool Unsigned(uint64& result)
        {
            Whitespace();
            if (_position >= _json.size() || _json[_position] < '0' || _json[_position] > '9')
                return false;
            bool leadingZero = _json[_position] == '0';
            uint64 value = 0;
            std::size_t digits = 0;
            while (_position < _json.size() && _json[_position] >= '0' && _json[_position] <= '9')
            {
                uint64 digit = uint64(_json[_position++] - '0');
                if ((leadingZero && digits) || value > (std::numeric_limits<uint64>::max() - digit) / 10)
                    return false;
                value = value * 10 + digit;
                ++digits;
            }
            result = value;
            return true;
        }

        bool HexScalar(uint32& scalar)
        {
            if (_json.size() - _position < 4)
                return false;
            scalar = 0;
            for (uint32 digit = 0; digit < 4; ++digit)
            {
                char c = _json[_position++];
                uint32 value = 0;
                if (c >= '0' && c <= '9') value = uint32(c - '0');
                else if (c >= 'a' && c <= 'f') value = uint32(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') value = uint32(c - 'A' + 10);
                else return false;
                scalar = (scalar << 4) | value;
            }
            return true;
        }

        static void AppendScalar(std::string& out, uint32 scalar)
        {
            if (scalar <= 0x7F)
                out.push_back(char(scalar));
            else if (scalar <= 0x7FF)
            {
                out.push_back(char(0xC0 | (scalar >> 6)));
                out.push_back(char(0x80 | (scalar & 0x3F)));
            }
            else if (scalar <= 0xFFFF)
            {
                out.push_back(char(0xE0 | (scalar >> 12)));
                out.push_back(char(0x80 | ((scalar >> 6) & 0x3F)));
                out.push_back(char(0x80 | (scalar & 0x3F)));
            }
            else
            {
                out.push_back(char(0xF0 | (scalar >> 18)));
                out.push_back(char(0x80 | ((scalar >> 12) & 0x3F)));
                out.push_back(char(0x80 | ((scalar >> 6) & 0x3F)));
                out.push_back(char(0x80 | (scalar & 0x3F)));
            }
        }

        bool String(std::string& result)
        {
            if (!Take('"'))
                return false;
            std::string value;
            while (_position < _json.size())
            {
                unsigned char c = static_cast<unsigned char>(_json[_position++]);
                if (c == '"')
                {
                    result = std::move(value);
                    return true;
                }
                if (c < 0x20)
                    return false;
                if (c != '\\')
                {
                    value.push_back(char(c));
                    continue;
                }
                if (_position == _json.size())
                    return false;
                switch (_json[_position++])
                {
                    case '"': value.push_back('"'); break;
                    case '\\': value.push_back('\\'); break;
                    case '/': value.push_back('/'); break;
                    case 'b': value.push_back('\b'); break;
                    case 'f': value.push_back('\f'); break;
                    case 'n': value.push_back('\n'); break;
                    case 'r': value.push_back('\r'); break;
                    case 't': value.push_back('\t'); break;
                    case 'u':
                    {
                        uint32 scalar = 0;
                        if (!HexScalar(scalar))
                            return false;
                        if (scalar >= 0xD800 && scalar <= 0xDBFF)
                        {
                            if (_json.size() - _position < 6 || _json[_position] != '\\' || _json[_position + 1] != 'u')
                                return false;
                            _position += 2;
                            uint32 low = 0;
                            if (!HexScalar(low) || low < 0xDC00 || low > 0xDFFF)
                                return false;
                            scalar = 0x10000 + ((scalar - 0xD800) << 10) + low - 0xDC00;
                        }
                        else if (scalar >= 0xDC00 && scalar <= 0xDFFF)
                            return false;
                        AppendScalar(value, scalar);
                        break;
                    }
                    default: return false;
                }
            }
            return false;
        }

        std::string_view _json;
        std::size_t _position = 0;
    };
}

bool ParseNpcAgentProfile(std::string_view json, NpcAgentProfile& profile)
{
    profile = {};
    if (json.empty() || json.size() > NpcAgentProfileCatalog::MaxProfileFileBytes)
        return false;
    return ProfileParser(json).Parse(profile);
}

bool NpcAgentProfileCatalog::Load(std::string const& directory)
{
    _profiles.clear();
    _lastError.clear();
    auto fail = [&](char const* reason) { _lastError = reason; return false; };
    std::error_code error;
    std::filesystem::directory_iterator entries(directory, error);
    if (error)
        return fail("profile directory unavailable");
    std::unordered_map<uint64, NpcAgentProfile> loaded;
    std::size_t scanned = 0;
    for (auto const end = std::filesystem::directory_iterator(); entries != end; entries.increment(error))
    {
        if (error)
            return fail("profile directory read failed");
        if (++scanned > MaxProfiles * 2)
            return fail("profile directory entry limit exceeded");
        if (entries->path().extension() != ".json")
            continue;
        auto status = entries->symlink_status(error);
        if (error || !std::filesystem::is_regular_file(status))
            return fail("profile file is not a regular file");
        auto size = entries->file_size(error);
        if (error || !size || size > MaxProfileFileBytes)
            return fail("profile file size invalid");
        if (loaded.size() >= MaxProfiles)
            return fail("profile count limit exceeded");
        std::ifstream input(entries->path(), std::ios::binary);
        if (!input)
            return fail("profile file unavailable");
        std::string body(MaxProfileFileBytes + 1, '\0');
        input.read(body.data(), std::streamsize(body.size()));
        std::size_t bytes = std::size_t(input.gcount());
        if (input.bad() || !bytes || bytes > MaxProfileFileBytes)
            return fail("profile file read failed");
        body.resize(bytes);
        NpcAgentProfile profile;
        if (!ParseNpcAgentProfile(body, profile))
            return fail("profile schema or identity invalid");
        uint64 id = profile.Agent.Value;
        if (!loaded.emplace(id, std::move(profile)).second)
            return fail("duplicate profile agent identity");
    }
    if (error)
        return fail("profile directory read failed");
    _profiles = std::move(loaded);
    return true;
}

NpcAgentProfile const* NpcAgentProfileCatalog::Get(AgentId id) const
{
    auto found = _profiles.find(id.Value);
    return found == _profiles.end() ? nullptr : &found->second;
}

NpcAgentCatalogIssues NpcAgentProfileCatalog::LoadRuntime(std::string const& directory,
    RuntimeValidator const& validator, std::size_t maxAccepted)
{
    _profiles.clear();
    _lastError.clear();
    NpcAgentCatalogIssues issues;
    maxAccepted = std::min(maxAccepted, MaxProfiles);
    for (auto const& path : NpcAgentCatalogLoading::Files(directory, MaxProfiles * 2, false, issues))
    {
        auto body = NpcAgentCatalogLoading::Read(path, MaxProfileFileBytes, issues);
        if (!body) continue;
        NpcAgentProfile profile;
        if (!ParseNpcAgentProfile(*body, profile))
        {
            issues.push_back({NpcAgentCatalogLoading::Filename(path), 0, "profile schema or identity invalid"});
            continue;
        }
        uint64 id = profile.Agent.Value;
        char const* failure = nullptr;
        if (path.filename() != std::filesystem::path(std::to_string(id) + ".json"))
            failure = "profile filename must equal spawn ID";
        else if (_profiles.find(id) != _profiles.end()) failure = "duplicate profile agent identity";
        else if (validator) failure = validator(profile);
        if (!failure && _profiles.size() >= maxAccepted) failure = "eligible profile limit exceeded";
        if (failure)
        {
            issues.push_back({NpcAgentCatalogLoading::Filename(path), id, failure});
            continue;
        }
        _profiles.emplace(id, std::move(profile));
    }
    if (!issues.empty()) _lastError = issues.front().Reason;
    return issues;
}
