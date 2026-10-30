/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/NpcAgentAppearance.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>

namespace
{
    std::string AppearanceJson(uint64 spawn = 901035, std::string const& gear = "[]")
    {
        return "{\"schema_version\":1,\"spawn_id\":" + std::to_string(spawn) +
            R"(,"map_id":725,"appearance":{"race":1,"gender":0,"class_id":1,"skin":2,"face":3,"hair_style":4,"hair_color":5,"facial_hair":6,"equipment":)" + gear + "}}";
    }
    std::string Replace(std::string text, std::string const& from, std::string const& to)
    { auto pos = text.find(from); REQUIRE(pos != std::string::npos); text.replace(pos, from.size(), to); return text; }
    struct AppearanceDirectory
    {
        AppearanceDirectory()
        {
            auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            for (uint32 attempt = 0; attempt < 100; ++attempt)
            {
                Path = std::filesystem::temp_directory_path() / ("aiworld-appearance-test-" + std::to_string(stamp) + "-" + std::to_string(attempt));
                std::error_code error;
                if (std::filesystem::create_directory(Path, error)) { Created = true; return; }
            }
        }
        ~AppearanceDirectory()
        { if (Created) { std::error_code error; std::filesystem::remove_all(Path, error); } }
        void Write(std::string const& name, std::string const& body)
        { std::ofstream file(Path / name, std::ios::binary); file << body; REQUIRE(bool(file)); }
        std::filesystem::path Path;
        bool Created = false;
    };
}

TEST_CASE("NPC appearance sidecars preserve uint64 identity and exact humanoid fields", "[AIWorld][NpcAgent][Appearance]")
{
    NpcAgentAppearance appearance;
    REQUIRE(ParseNpcAgentAppearance(AppearanceJson(std::numeric_limits<uint64>::max(),
        R"([{"slot":"chest","item_id":38,"display_id":9891},{"slot":"main_hand","item_id":25,"display_id":1542}])"), appearance));
    CHECK(appearance.SpawnId == std::numeric_limits<uint64>::max());
    CHECK(appearance.MapId == 725);
    CHECK(appearance.Race == 1);
    CHECK(appearance.Gender == 0);
    CHECK(appearance.Class == 1);
    CHECK(appearance.Skin == 2);
    CHECK(appearance.Face == 3);
    CHECK(appearance.HairStyle == 4);
    CHECK(appearance.HairColor == 5);
    CHECK(appearance.FacialHair == 6);
    CHECK(appearance.Equipment[std::size_t(NpcAppearanceSlot::Chest)].ItemId == 38);
    CHECK(appearance.Equipment[std::size_t(NpcAppearanceSlot::Chest)].DisplayId == 9891);
    CHECK(appearance.Equipment[std::size_t(NpcAppearanceSlot::MainHand)].ItemId == 25);
    CHECK(appearance.Equipment[std::size_t(NpcAppearanceSlot::Head)].ItemId == 0);
    REQUIRE(ParseNpcAgentAppearance(AppearanceJson(), appearance));
    CHECK(appearance.Equipment[std::size_t(NpcAppearanceSlot::MainHand)].ItemId == 0);
}

TEST_CASE("NPC appearance sidecars reject coercion duplicate fields unknown fields and invalid bounds", "[AIWorld][NpcAgent][Appearance]")
{
    auto json = AppearanceJson();
    for (auto const& bad : {Replace(json, "901035", "0"), Replace(json, "901035", "18446744073709551616"),
        Replace(json, "901035", "\"901035\""), Replace(json, "901035", "1.5"), Replace(json, "901035", "01"),
        Replace(json, "901035", "-1"), Replace(json, "725", "4294967296"),
        Replace(json, "\"race\":1", "\"race\":9"), Replace(json, "\"gender\":0", "\"gender\":2"),
        Replace(json, "\"class_id\":1", "\"class_id\":6"), Replace(json, "\"face\":3", "\"face\":256"),
        Replace(json, "\"face\":3", "\"skin\":3"), Replace(json, "\"face\":3", "\"unknown\":3"),
        Replace(json, "\"schema_version\":1", "\"schema_version\":2"), json + "false", json.substr(0, json.size() - 1),
        std::string(NpcAgentAppearanceCatalog::MaxFileBytes + 1, ' ')})
    {
        NpcAgentAppearance appearance;
        appearance.SpawnId = 123;
        CHECK_FALSE(ParseNpcAgentAppearance(bad, appearance));
        CHECK(appearance.SpawnId == 0);
    }
    for (auto const& gear : {R"([{"slot":"chest","item_id":0,"display_id":1}])",
        R"([{"slot":"chest","item_id":1,"display_id":0}])", R"([{"slot":"invented","item_id":1,"display_id":1}])",
        R"([{"slot":"head","item_id":1,"display_id":1},{"slot":"head","item_id":2,"display_id":2}])",
        R"([{"slot":"head","item_id":1,"display_id":1,"hidden":0}])", R"([{"slot":"head","item_id":1}])",
        R"([{"slot":"head","item_id":1,"display_id":4294967296}])", R"([{"slot":"head","item_id":1,"display_id":1},])"})
    { NpcAgentAppearance appearance; CHECK_FALSE(ParseNpcAgentAppearance(AppearanceJson(901035, gear), appearance)); }
}

TEST_CASE("NPC appearance inventory slots keep clothing and weapon semantics separate", "[AIWorld][NpcAgent][Appearance]")
{
    CHECK(NpcAppearanceSlotAccepts(NpcAppearanceSlot::Chest, 5));
    CHECK(NpcAppearanceSlotAccepts(NpcAppearanceSlot::Chest, 20));
    CHECK(NpcAppearanceSlotAccepts(NpcAppearanceSlot::MainHand, 17));
    CHECK(NpcAppearanceSlotAccepts(NpcAppearanceSlot::OffHand, 14));
    CHECK(NpcAppearanceSlotAccepts(NpcAppearanceSlot::Ranged, 26));
    CHECK_FALSE(NpcAppearanceSlotAccepts(NpcAppearanceSlot::Chest, 13));
    CHECK_FALSE(NpcAppearanceSlotAccepts(NpcAppearanceSlot::OffHand, 17));
    CHECK_FALSE(NpcAppearanceSlotAccepts(NpcAppearanceSlot::Head, 0));
    CHECK_FALSE(NpcAppearanceSlotAccepts(NpcAppearanceSlot::Count, 1));
}

TEST_CASE("Optional appearance directories are atomic and bound to authored filenames", "[AIWorld][NpcAgent][Appearance]")
{
    AppearanceDirectory directory;
    REQUIRE(directory.Created);
    NpcAgentAppearanceCatalog catalog;
    CHECK(catalog.Load((directory.Path / "missing").string()));
    CHECK(catalog.Size() == 0);
    CHECK(catalog.Load(directory.Path.string()));
    directory.Write(".gitkeep", "");
    directory.Write("901035.json", AppearanceJson());
    directory.Write("901036.json", AppearanceJson(901036));
    REQUIRE(catalog.Load(directory.Path.string()));
    REQUIRE(catalog.Size() == 2);
    REQUIRE(catalog.Get(901035));
    CHECK(catalog.Get(901035)->SpawnId == 901035);
    CHECK_FALSE(catalog.Get(42));
    directory.Write("901036.json", AppearanceJson(901035));
    CHECK_FALSE(catalog.Load(directory.Path.string()));
    CHECK(catalog.Size() == 0);
    directory.Write("901036.json", AppearanceJson(901036));
    directory.Write("901037.json", std::string(NpcAgentAppearanceCatalog::MaxFileBytes + 1, ' '));
    CHECK_FALSE(catalog.Load(directory.Path.string()));
    CHECK(catalog.Size() == 0);
}
