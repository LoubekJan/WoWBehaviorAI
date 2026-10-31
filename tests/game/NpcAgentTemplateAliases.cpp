/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/NpcAgentTemplateAliases.h"
#include "Agent/LivingRolePolicy.h"

namespace
{
    NpcAgentTemplateAlias Alias(uint64 spawn = 901014, uint32 base = 11328, std::string name = "Sasa")
    { return {spawn, 725, NpcAgentTemplateEntry(spawn), base, std::move(name), "Resident"}; }
    NpcAgentTemplateBinding Binding(NpcAgentTemplateAlias const& alias)
    { return {alias.SpawnId, alias.MapId, alias.Entry, true, true, true, true, true}; }
}

TEST_CASE("Character native templates preserve individual names and original living roles", "[aiworld][npc][template]")
{
    NpcAgentTemplateAliases aliases;
    auto first = Alias();
    auto second = Alias(901035, 11328, "Matej");
    REQUIRE(aliases.Add(first, Binding(first)));
    REQUIRE(aliases.Add(second, Binding(second)));
    REQUIRE(aliases.Resolve(725, 901014, first.Entry) == 11328);
    REQUIRE(aliases.Resolve(725, 901035, second.Entry) == 11328);
    REQUIRE(aliases.Find(725, 901014, first.Entry)->Name == "Sasa");
    REQUIRE(aliases.Find(725, 901035, second.Entry)->Name == "Matej");
    REQUIRE(LivingRolePolicy::Resolve(AgentType::Civilian, first.Entry, false) == LivingRolePolicy::Role::Civilian);
    REQUIRE(LivingRolePolicy::Resolve(AgentType::Civilian, aliases.Resolve(725, 901014, first.Entry), false)
        == LivingRolePolicy::Role::Worker);
    // A shared source is not a shared presentation identity, and unrelated
    // native actors never acquire another character's classification alias.
    REQUIRE(aliases.Resolve(725, 901035, first.Entry) == first.Entry);
    REQUIRE(aliases.Resolve(0, 901014, first.Entry) == first.Entry);
    REQUIRE(aliases.Resolve(725, 901014, 11328) == 11328);
    REQUIRE_FALSE(aliases.Add(first, Binding(first)));
    REQUIRE(aliases.ProfileFailure(725, 901014, first.Entry, "Sasa") == nullptr);
    REQUIRE(aliases.ProfileFailure(725, 901014, first.Entry, "Changed uninstalled card") != nullptr);
    REQUIRE(aliases.ProfileFailure(725, 901035, first.Entry, "Sasa") != nullptr);
    REQUIRE(aliases.ProfileFailure(725, 901014, 11328, "Legacy card") == nullptr);
    // A bad optional card is isolated; installed ordinary source behavior is
    // retained rather than reverting the clone to an unrelated entry policy.
    REQUIRE(aliases.Resolve(725, 901014, first.Entry) == 11328);
}

TEST_CASE("Character template aliases reject foreign or unverified native bindings", "[aiworld][npc][template]")
{
    auto valid = Alias();
    auto binding = Binding(valid);
    REQUIRE(NpcAgentTemplateFailure(valid, binding) == nullptr);
    for (uint32 field = 0; field < 8; ++field)
    {
        auto candidate = binding;
        switch (field)
        {
            case 0: candidate.AgentId = 901015; break;
            case 1: candidate.MapId = 0; break;
            case 2: candidate.ActualEntry = 11328; break;
            case 3: candidate.Controlled = false; break;
            case 4: candidate.InMembership = false; break;
            case 5: candidate.Participating = false; break;
            case 6: candidate.NativeTemplatesMatch = false; break;
            case 7: candidate.KnownSource = false; break;
        }
        NpcAgentTemplateAliases aliases;
        REQUIRE(NpcAgentTemplateFailure(valid, candidate) != nullptr);
        REQUIRE_FALSE(aliases.Add(valid, candidate));
        REQUIRE(aliases.Size() == 0);
    }
    for (auto candidate : {Alias(900999), Alias(901100), Alias(901014, 9800000), Alias(901014, 0)})
        REQUIRE(NpcAgentTemplateFailure(candidate, Binding(candidate)) != nullptr);
    auto candidate = valid; candidate.Entry = 9800015;
    REQUIRE(NpcAgentTemplateFailure(candidate, Binding(candidate)) != nullptr);
}

TEST_CASE("Character template title limits match the exported native descriptor", "[aiworld][npc][template]")
{
    auto alias = Alias();
    alias.Title.clear();
    REQUIRE(NpcAgentTemplateFailure(alias, Binding(alias)) == nullptr);
    alias.Title = std::string(80, 'a');
    REQUIRE(NpcAgentTemplateFailure(alias, Binding(alias)) == nullptr);
    alias.Title.push_back('a');
    REQUIRE(NpcAgentTemplateFailure(alias, Binding(alias)) != nullptr);
    alias.Title = "Title\nwith control";
    REQUIRE(NpcAgentTemplateFailure(alias, Binding(alias)) != nullptr);
    alias.Title = "|cffffffffFormatted";
    REQUIRE(NpcAgentTemplateFailure(alias, Binding(alias)) != nullptr);
    alias = Alias(); alias.Name = "";
    REQUIRE(NpcAgentTemplateFailure(alias, Binding(alias)) != nullptr);
}

TEST_CASE("Character aliases preserve source predator prey guard and service classification", "[aiworld][npc][template]")
{
    struct Example { uint32 Entry; AgentType Type; LivingRolePolicy::Role Role; };
    for (auto const& example : {Example{883, AgentType::Prey, LivingRolePolicy::Role::Prey},
        {299, AgentType::Predator, LivingRolePolicy::Role::Predator},
        {1423, AgentType::Guard, LivingRolePolicy::Role::Guard},
        {66, AgentType::Merchant, LivingRolePolicy::Role::Service},
        {589, AgentType::Combatant, LivingRolePolicy::Role::Combatant}})
    {
        NpcAgentTemplateAliases aliases;
        auto alias = Alias(901014, example.Entry);
        REQUIRE(aliases.Add(alias, Binding(alias)));
        REQUIRE(LivingRolePolicy::Resolve(example.Type, aliases.Resolve(725, alias.SpawnId, alias.Entry), false) == example.Role);
    }
}

TEST_CASE("Character name cache versions are deterministic and change with authored labels", "[aiworld][npc][template]")
{
    NpcAgentTemplateAliases none, first, reordered, renamed, retitled;
    REQUIRE(none.ClientCacheVersion(0) == 0);
    REQUIRE(none.ClientCacheVersion(1234) == 1234);
    auto a = Alias(), b = Alias(901035, 11328, "Matej");
    REQUIRE(first.Add(a, Binding(a))); REQUIRE(first.Add(b, Binding(b)));
    REQUIRE(reordered.Add(b, Binding(b))); REQUIRE(reordered.Add(a, Binding(a)));
    REQUIRE(first.ClientCacheVersion(1234) == reordered.ClientCacheVersion(1234));
    REQUIRE(first.ClientCacheVersion(1234) != first.ClientCacheVersion(5678));
    auto changed = a; changed.Name = "Sasa nova";
    REQUIRE(renamed.Add(changed, Binding(changed))); REQUIRE(renamed.Add(b, Binding(b)));
    changed = a; changed.Title = "A different role";
    REQUIRE(retitled.Add(changed, Binding(changed))); REQUIRE(retitled.Add(b, Binding(b)));
    REQUIRE(first.ClientCacheVersion(1234) != renamed.ClientCacheVersion(1234));
    REQUIRE(first.ClientCacheVersion(1234) != retitled.ClientCacheVersion(1234));
    REQUIRE(first.ClientCacheVersion(1234) > 0);
    REQUIRE(first.ClientCacheVersion(1234) <= 0x7fffffffu);
}

TEST_CASE("Character aliases cover all reviewed actors independently of optional model capacity", "[aiworld][npc][template]")
{
    NpcAgentTemplateAliases aliases;
    for (uint64 id = 901000; id <= 901099; ++id)
    {
        auto alias = Alias(id);
        REQUIRE(aliases.Add(alias, Binding(alias)));
    }
    REQUIRE(aliases.Size() == 100);
    auto outside = Alias(901100);
    REQUIRE_FALSE(aliases.Add(outside, Binding(outside)));
    aliases.Clear();
    REQUIRE(aliases.Size() == 0);
    REQUIRE(aliases.ClientCacheVersion(1234) == 1234);
    REQUIRE(aliases.Resolve(725, 901014, 9800014) == 9800014);
}
