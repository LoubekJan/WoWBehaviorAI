/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/NpcAgentProfile.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace
{
    std::string ProfileJson(uint64 id = 901035)
    {
        return "{\"agent_id\":" + std::to_string(id) + ",\"map_id\":0,\"spawn_id\":" + std::to_string(id)
            + R"(,"name":"Matej","backstory":"Pracuji ve vesnici.","personality":"Jsem klidny."})";
    }

    struct ProfileDirectory
    {
        ProfileDirectory()
        {
            auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            for (uint32 attempt = 0; attempt < 100; ++attempt)
            {
                Path = std::filesystem::temp_directory_path()
                    / ("aiworld-npc-profile-test-" + std::to_string(stamp) + "-" + std::to_string(attempt));
                std::error_code error;
                if (std::filesystem::create_directory(Path, error))
                {
                    Created = true;
                    break;
                }
            }
        }
        ~ProfileDirectory()
        {
            if (Created)
            {
                // Only the exact dedicated directory successfully created
                // above is owned and removed by this fixture.
                std::error_code error;
                std::filesystem::remove_all(Path, error);
            }
        }
        void Write(std::string const& name, std::string const& body) const
        {
            std::ofstream file(Path / name, std::ios::binary);
            file << body;
            REQUIRE(bool(file));
        }
        std::filesystem::path Path;
        bool Created = false;
    };
}

TEST_CASE("NPC profiles require exact individual identity and six schema fields", "[AIWorld][NpcAgent]")
{
    NpcAgentProfile profile;
    REQUIRE(ParseNpcAgentProfile(ProfileJson(), profile));
    CHECK(profile.Agent.Value == 901035);
    CHECK(profile.SpawnId == 901035);
    CHECK(profile.MapId == 0);
    CHECK(profile.Name == "Matej");
    CHECK(profile.Personality == "Jsem klidny.");
    CHECK_FALSE(ParseNpcAgentProfile("{\"agent_id\":901035}", profile));
    CHECK_FALSE(profile.Agent);
    CHECK_FALSE(ParseNpcAgentProfile(ProfileJson() + " true", profile));
    CHECK_FALSE(ParseNpcAgentProfile(ProfileJson(0), profile));
    CHECK_FALSE(ParseNpcAgentProfile(R"({"agent_id":1,"map_id":0,"spawn_id":2,"name":"N","backstory":"B","personality":"P"})", profile));
    CHECK_FALSE(ParseNpcAgentProfile(R"({"agent_id":1,"map_id":0,"spawn_id":1,"name":"N","backstory":"B","name":"P"})", profile));
    CHECK_FALSE(ParseNpcAgentProfile(R"({"agent_id":1,"map_id":0,"spawn_id":1,"name":"N","backstory":"B","personality":"P","entry":1})", profile));
    CHECK_FALSE(ParseNpcAgentProfile(R"({"agent_id":01,"map_id":0,"spawn_id":1,"name":"N","backstory":"B","personality":"P"})", profile));
    CHECK_FALSE(ParseNpcAgentProfile(R"({"agent_id":1.0,"map_id":0,"spawn_id":1,"name":"N","backstory":"B","personality":"P"})", profile));
    CHECK_FALSE(ParseNpcAgentProfile(R"({"agent_id":1,"map_id":4294967296,"spawn_id":1,"name":"N","backstory":"B","personality":"P"})", profile));
    CHECK_FALSE(ParseNpcAgentProfile(R"({"agent_id":18446744073709551616,"map_id":0,"spawn_id":1,"name":"N","backstory":"B","personality":"P"})", profile));
}

TEST_CASE("NPC profile parser decodes legal JSON Unicode and rejects unsafe text", "[AIWorld][NpcAgent]")
{
    NpcAgentProfile profile;
    REQUIRE(ParseNpcAgentProfile(R"({"personality":"Klidny","backstory":"Znam \"sklad\".","name":"B\u011bla","spawn_id":1,"map_id":0,"agent_id":1})", profile));
    CHECK(profile.Name == "B\xC4\x9Bla");
    CHECK(profile.Backstory == "Znam \"sklad\".");
    REQUIRE(ParseNpcAgentProfile(R"({"agent_id":1,"map_id":0,"spawn_id":1,"name":"N","backstory":"\ud83c\udf5e","personality":"P"})", profile));
    CHECK(profile.Backstory == "\xF0\x9F\x8D\x9E");
    for (std::string const& name : { R"(\ud800)", R"(\udc00)", R"(\ud800\u0041)",
        R"(\n)", R"(\u0000)", R"(\u202e)", "|cffffffffName|r", "" })
    {
        auto json = ProfileJson();
        json.replace(json.find("Matej"), 5, name);
        CHECK_FALSE(ParseNpcAgentProfile(json, profile));
    }
    auto invalidUtf8 = ProfileJson();
    invalidUtf8.replace(invalidUtf8.find("Matej"), 5, "\xC0\xAF");
    CHECK_FALSE(ParseNpcAgentProfile(invalidUtf8, profile));
    CHECK_FALSE(ParseNpcAgentProfile(std::string(NpcAgentProfileCatalog::MaxProfileFileBytes + 1, ' '), profile));
}

TEST_CASE("NPC profile lore supports bounded authored lines and normalizes CRLF", "[AIWorld][NpcAgent]")
{
    auto json = [](std::string const& backstory, std::string const& personality)
    {
        return R"({"agent_id":1,"map_id":725,"spawn_id":1,"name":"N","backstory":")"
            + backstory + R"(","personality":")" + personality + R"("})";
    };
    NpcAgentProfile profile;
    REQUIRE(ParseNpcAgentProfile(json(R"(Prvni\r\nDruhy\n\tTreti)", R"(Klidny\nVstricny)"), profile));
    CHECK(profile.Backstory == "Prvni\nDruhy\n\tTreti");
    CHECK(profile.Personality == "Klidny\nVstricny");
    CHECK_FALSE(ParseNpcAgentProfile(json(R"(Jedna\rDva)", "Klidny"), profile));
    CHECK_FALSE(ParseNpcAgentProfile(json(R"(Jedna\bDva)", "Klidny"), profile));
    REQUIRE(ParseNpcAgentProfile(json(std::string(8192, 'b'), std::string(2048, 'p')), profile));
    CHECK(profile.Backstory.size() == 8192);
    CHECK(profile.Personality.size() == 2048);
    CHECK_FALSE(ParseNpcAgentProfile(json(std::string(8193, 'b'), "p"), profile));
    CHECK_FALSE(ParseNpcAgentProfile(json("b", std::string(2049, 'p')), profile));
    std::string twoByte;
    for (std::size_t i = 0; i < 4096; ++i)
        twoByte += "\xC4\x9B";
    REQUIRE(ParseNpcAgentProfile(json(twoByte, "p"), profile));
    twoByte += "a";
    CHECK_FALSE(ParseNpcAgentProfile(json(twoByte, "p"), profile));
    CHECK(NpcAgentProfileCatalog::MaxProfileFileBytes == 16384);
}

TEST_CASE("NPC profile catalog rejects a bad file or duplicate without retaining partial or old profiles", "[AIWorld][NpcAgent]")
{
    ProfileDirectory directory;
    REQUIRE(directory.Created);
    directory.Write("901035.json", ProfileJson());
    directory.Write("901036.json", ProfileJson(901036));
    NpcAgentProfileCatalog catalog;
    REQUIRE(catalog.Load(directory.Path.string()));
    CHECK(catalog.Size() == 2);
    REQUIRE(catalog.Get(AgentId{901035}));
    CHECK(catalog.Get(AgentId{901035})->Name == "Matej");
    CHECK_FALSE(catalog.Get(AgentId{999}));
    directory.Write("901036.json", ProfileJson());
    CHECK_FALSE(catalog.Load(directory.Path.string()));
    CHECK(catalog.Size() == 0);
    CHECK(catalog.GetLastError() == "duplicate profile agent identity");
    directory.Write("901036.json", "{broken}");
    CHECK_FALSE(catalog.Load(directory.Path.string()));
    CHECK(catalog.Size() == 0);
    CHECK(catalog.GetLastError() == "profile schema or identity invalid");
    directory.Write("901036.json", std::string(NpcAgentProfileCatalog::MaxProfileFileBytes + 1, 'x'));
    CHECK_FALSE(catalog.Load(directory.Path.string()));
    CHECK(catalog.Size() == 0);
    CHECK(catalog.GetLastError().find(directory.Path.string()) == std::string::npos);
    CHECK_FALSE(catalog.Load((directory.Path / "absent").string()));
    CHECK(catalog.Size() == 0);
}
