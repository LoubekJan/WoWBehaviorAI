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
    std::string ProfileJson(uint64 id = 901035, uint32 map = 0)
    {
        return "{\"agent_id\":" + std::to_string(id) + ",\"map_id\":" + std::to_string(map) + ",\"spawn_id\":" + std::to_string(id)
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

TEST_CASE("Runtime profile imports isolate unregistered civilian and malformed cards from healthy agents", "[AIWorld][NpcAgent][Import]")
{
    ProfileDirectory directory;
    REQUIRE(directory.Created);
    directory.Write("901035.json", ProfileJson(901035, 725));
    directory.Write("901036.json", ProfileJson(901036, 725));
    // The real failed import targeted this native city civilian, which has no
    // registry identity. A syntactically valid card grants it no new authority.
    directory.Write("901014.json", ProfileJson(901014, 725));
    directory.Write("901037.json", "{broken}");
    directory.Write("character-studio-draft.json", R"({"card":{"name":"Draft"}})");
    directory.Write("901035 (1).json", ProfileJson(901035, 725));
    NpcAgentProfileCatalog catalog;
    auto issues = catalog.LoadRuntime(directory.Path.string(), [](NpcAgentProfile const& profile)
    {
        if (profile.Agent.Value == 901014) return NpcAgentProfileBindingFailure(profile, nullptr);
        NpcAgentProfileBinding binding{profile.Agent, 725, profile.SpawnId, true, true, true, true};
        return NpcAgentProfileBindingFailure(profile, &binding);
    });
    REQUIRE(catalog.Size() == 2);
    CHECK(catalog.Get(AgentId{901035}));
    CHECK(catalog.Get(AgentId{901036}));
    CHECK_FALSE(catalog.Get(AgentId{901014}));
    REQUIRE(issues.size() == 4);
    auto invalid = std::find_if(issues.begin(), issues.end(), [](auto const& issue) { return issue.Identity == 901014; });
    REQUIRE(invalid != issues.end());
    CHECK(invalid->Filename == "901014.json");
    CHECK(invalid->Reason == "agent is not registered");
    for (auto const& issue : issues)
    {
        CHECK(issue.Filename.find(directory.Path.string()) == std::string::npos);
        CHECK(issue.Reason.find("Pracuji") == std::string::npos);
    }
}

TEST_CASE("NPC authoring eligibility preserves every existing runtime ownership boundary", "[AIWorld][NpcAgent][Import]")
{
    NpcAgentProfile profile;
    REQUIRE(ParseNpcAgentProfile(ProfileJson(901014, 725), profile));
    CHECK(std::string(NpcAgentProfileBindingFailure(profile, nullptr)) == "agent is not registered");
    NpcAgentProfileBinding native{AgentId{901014}, 725, 901014, false, false, false, true};
    CHECK(std::string(NpcAgentProfileBindingFailure(profile, &native)) == "agent is not AIWorld controlled");
    CHECK_FALSE(native.Controlled); // validation never promotes a native NPC
    NpcAgentProfileBinding valid{profile.Agent, profile.MapId, profile.SpawnId, true, true, true, true};
    CHECK(NpcAgentProfileBindingFailure(profile, &valid) == nullptr);
    for (int boundary = 0; boundary < 7; ++boundary)
    {
        auto blocked = valid;
        if (boundary == 0) blocked.Agent = AgentId{901035};
        if (boundary == 1) blocked.SpawnId = 901035;
        if (boundary == 2) blocked.MapId = 0;
        if (boundary == 3) blocked.Controlled = false;
        if (boundary == 4) blocked.InMembership = false;
        if (boundary == 5) blocked.Participating = false;
        if (boundary == 6) blocked.SupportedRole = false;
        CHECK(NpcAgentProfileBindingFailure(profile, &blocked) != nullptr);
    }
}

TEST_CASE("Runtime profile capacity applies after eligibility with deterministic canonical priority", "[AIWorld][NpcAgent][Import]")
{
    ProfileDirectory directory;
    REQUIRE(directory.Created);
    // Create in reverse order so an unordered filesystem cannot choose which
    // eligible profiles fill the controller's bounded slot budget.
    directory.Write("901037.json", ProfileJson(901037));
    directory.Write("901036.json", ProfileJson(901036));
    directory.Write("901035.json", ProfileJson());
    directory.Write("901014.json", ProfileJson(901014));
    NpcAgentProfileCatalog catalog;
    auto issues = catalog.LoadRuntime(directory.Path.string(), [](NpcAgentProfile const& profile) -> char const*
    { return profile.SpawnId == 901014 ? "agent is not registered" : nullptr; }, 2);
    REQUIRE(catalog.Size() == 2);
    CHECK(catalog.Get(AgentId{901035}));
    CHECK(catalog.Get(AgentId{901036}));
    CHECK_FALSE(catalog.Get(AgentId{901037}));
    REQUIRE(issues.size() == 2);
    CHECK(issues[0].Identity == 901014);
    CHECK(issues[1].Identity == 901037);
    CHECK(issues[1].Reason == "eligible profile limit exceeded");
    auto again = catalog.LoadRuntime(directory.Path.string(), [](NpcAgentProfile const& profile) -> char const*
    { return profile.SpawnId == 901014 ? "agent is not registered" : nullptr; }, 2);
    REQUIRE(again.size() == issues.size());
    for (std::size_t i = 0; i < issues.size(); ++i) CHECK(again[i].Filename == issues[i].Filename);
}

TEST_CASE("Missing or entirely rejected runtime authoring leaves an empty optional controller catalog", "[AIWorld][NpcAgent][Import]")
{
    ProfileDirectory directory;
    REQUIRE(directory.Created);
    NpcAgentProfileCatalog catalog;
    auto missing = catalog.LoadRuntime((directory.Path / "absent").string());
    REQUIRE(missing.size() == 1);
    CHECK(catalog.Size() == 0);
    CHECK(missing[0].Reason == "directory unavailable");
    directory.Write("901014.json", ProfileJson(901014, 725));
    auto invalid = catalog.LoadRuntime(directory.Path.string(), [](NpcAgentProfile const&) -> char const*
    { return "agent is not registered"; });
    REQUIRE(invalid.size() == 1);
    CHECK(catalog.Size() == 0);
}

TEST_CASE("Runtime authoring directory scans and diagnostic names stay bounded", "[AIWorld][NpcAgent][Import]")
{
    ProfileDirectory directory;
    REQUIRE(directory.Created);
    directory.Write("901035.json", ProfileJson());
    for (std::size_t i = 0; i < NpcAgentProfileCatalog::MaxProfiles * 2; ++i)
        directory.Write("note-" + std::to_string(i) + ".txt", "unrelated");
    NpcAgentProfileCatalog catalog;
    auto issues = catalog.LoadRuntime(directory.Path.string());
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].Reason == "directory entry limit exceeded");
    CHECK(catalog.Size() == 0);
    std::string unsafe = "line|break";
    unsafe.push_back('\n');
    unsafe += std::string(100, 'z');
    auto display = NpcAgentCatalogLoading::Filename(std::filesystem::path(unsafe));
    CHECK(display.size() <= 80);
    CHECK(display.find('|') == std::string::npos);
    CHECK(display.find('\n') == std::string::npos);
}
