/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/LivingSupplyState.h"
#include "Agent/LivingSupplyRuntime.h"
#include <limits>
#include <ostream>

namespace LivingSupply
{
    std::ostream& operator<<(std::ostream& out, Token const& token)
    { return out << token.Actor << ':' << token.Serial; }
}

namespace
{
    using namespace LivingSupply;
    Ledger SupplyFixture(uint32 initial = 0)
    {
        Ledger ledger;
        REQUIRE(ledger.ConfigureDepot(1, initial));
        REQUIRE(ledger.ConfigureDepot(2));
        REQUIRE(ledger.RegisterActor(101, 1));
        REQUIRE(ledger.RegisterActor(102, 2));
        REQUIRE(ledger.RegisterActor(103, 1));
        REQUIRE(ledger.RegisterActor(201, 0, 0, true));
        REQUIRE(ledger.RegisterActor(202, 0, 0, true));
        return ledger;
    }
    Ledger RaidFixture(uint32 initial = 6)
    {
        auto ledger = SupplyFixture(initial);
        REQUIRE(ledger.ConfigureDepot(3));
        REQUIRE(ledger.RegisterRaider(301));
        return ledger;
    }
    void Conserved(Ledger const& ledger)
    {
        REQUIRE(ledger.Conserved());
        REQUIRE(ledger.PotentialConserved());
    }
    void Harvest(Ledger& ledger, uint64 actor, CarcassKey key, uint32 amount = 6)
    {
        REQUIRE(ledger.RegisterCarcass(key, 1, 50000));
        auto token = ledger.Reserve(actor, key, ClaimKind::Harvest, amount, 2);
        REQUIRE(token);
        auto result = ledger.Commit(*token, 5002);
        REQUIRE(result.Success);
        REQUIRE_FALSE(result.Duplicate);
        REQUIRE(result.Portions == amount);
        Conserved(ledger);
    }
}

TEST_CASE("Finite carcass food passes through cargo and depot before a completed meal", "[AIWorld][LivingSupply]")
{
    auto ledger = SupplyFixture(2);
    Harvest(ledger, 101, {300, 1});
    REQUIRE(ledger.GetCargo(101) == 6);
    REQUIRE(ledger.GetDepot(1)->Stock == 2);
    REQUIRE(ledger.GetCounters().HarvestedTotal == 6);
    REQUIRE_FALSE(ledger.Deliver(101, 10, 1, 6, false).Success);
    REQUIRE(ledger.GetCargo(101) == 6);
    REQUIRE(ledger.GetDepot(1)->Stock == 2);
    auto delivery = ledger.Deliver(101, 10, 1, 6, true);
    REQUIRE(delivery.Success);
    REQUIRE(delivery.Portions == 6);
    REQUIRE(ledger.GetCargo(101) == 0);
    REQUIRE(ledger.GetDepot(1)->Stock == 8);
    REQUIRE(ledger.GetCounters().DeliveredTotal == 6);
    REQUIRE_FALSE(ledger.EatDepot(101, 11, 1, 1, false).Success);
    REQUIRE(ledger.GetCounters().ConsumedTotal == 0);
    auto meal = ledger.EatDepot(101, 11, 1, 1, true);
    REQUIRE(meal.Success);
    REQUIRE(meal.Portions == 1);
    REQUIRE(ledger.GetDepot(1)->Stock == 7);
    REQUIRE(ledger.GetCounters().ConsumedTotal == 1);
    REQUIRE(ledger.GetCounters().DepotConsumed == 1);
    REQUIRE(ledger.ConfigureDepot(1, 2));
    REQUIRE(ledger.RegisterActor(101, 1));
    REQUIRE(ledger.GetDepot(1)->Stock == 7);
    REQUIRE(ledger.GetCounters().InitialFood == 2);
    Conserved(ledger);
}

TEST_CASE("Hunters and predators reserve different portions of one finite corpse", "[AIWorld][LivingSupply]")
{
    auto ledger = SupplyFixture();
    CarcassKey key{300, 1};
    REQUIRE(ledger.RegisterCarcass(key, 1, 10000));
    auto hunter = ledger.Reserve(101, key, ClaimKind::Harvest, 3, 10);
    auto predator = ledger.Reserve(201, key, ClaimKind::PredatorMeal, 3, 10);
    REQUIRE(hunter);
    REQUIRE(predator);
    REQUIRE(ledger.GetCarcass(key)->Remaining == 6);
    REQUIRE(ledger.GetCarcass(key)->Reserved == 6);
    REQUIRE_FALSE(ledger.Reserve(102, key, ClaimKind::Harvest, 6, 10));
    REQUIRE_FALSE(ledger.Reserve(202, key, ClaimKind::PredatorMeal, 3, 10));
    REQUIRE(ledger.GetCounters().HarvestedTotal == 0);
    Conserved(ledger);
    auto h = ledger.Commit(*hunter, 5010);
    REQUIRE(h.Success);
    REQUIRE(h.Portions == 3);
    REQUIRE(ledger.GetCarcass(key)->Reserved == 3);
    auto p = ledger.Commit(*predator, 5010);
    REQUIRE(p.Success);
    REQUIRE(p.Portions == 3);
    REQUIRE(ledger.GetCarcass(key)->Remaining == 0);
    REQUIRE(ledger.GetCarcass(key)->Reserved == 0);
    REQUIRE(ledger.GetCargo(101) == 3);
    REQUIRE(ledger.GetCargo(201) == 0);
    REQUIRE(ledger.GetCounters().HarvestedTotal == 6);
    REQUIRE(ledger.GetCounters().ConsumedTotal == 3);
    REQUIRE(ledger.GetCounters().PredatorConsumed == 3);
    REQUIRE_FALSE(ledger.Reserve(201, key, ClaimKind::PredatorMeal, 3, 6000));
    Conserved(ledger);
}

TEST_CASE("Repeated completions return receipts without duplicating food or consuming a newer load", "[AIWorld][LivingSupply]")
{
    auto ledger = SupplyFixture();
    CarcassKey key{300, 1};
    REQUIRE(ledger.RegisterCarcass(key, 1, 50000));
    auto token = ledger.Reserve(101, key, ClaimKind::Harvest, 6, 2);
    REQUIRE(token);
    REQUIRE(ledger.Commit(*token, 5002).Success);
    auto repeat = ledger.Commit(*token, 5003);
    REQUIRE(repeat.Success);
    REQUIRE(repeat.Duplicate);
    REQUIRE(repeat.Portions == 6);
    REQUIRE(ledger.GetCounters().HarvestedTotal == 6);
    REQUIRE(ledger.Deliver(101, 20, 1, 6, true).Success);
    Harvest(ledger, 101, {300, 2});
    auto repeatedDelivery = ledger.Deliver(101, 20, 1, 6, true);
    REQUIRE(repeatedDelivery.Success);
    REQUIRE(repeatedDelivery.Duplicate);
    REQUIRE(repeatedDelivery.Portions == 6);
    REQUIRE(ledger.GetCargo(101) == 6);
    REQUIRE(ledger.GetDepot(1)->Stock == 6);
    REQUIRE_FALSE(ledger.Deliver(101, 20, 1, 3, true).Success);
    REQUIRE_FALSE(ledger.Deliver(101, 19, 1, 6, true).Success);
    REQUIRE_FALSE(ledger.EatDepot(101, 20, 1, 1, true).Success);
    REQUIRE(ledger.EatDepot(101, 21, 1, 1, true).Success);
    auto repeatedMeal = ledger.EatDepot(101, 21, 1, 1, true);
    REQUIRE(repeatedMeal.Success);
    REQUIRE(repeatedMeal.Duplicate);
    REQUIRE(ledger.GetDepot(1)->Stock == 5);
    REQUIRE(ledger.GetCounters().ConsumedTotal == 1);
    REQUIRE_FALSE(ledger.Deliver(101, 20, 1, 6, true).Success);
    Conserved(ledger);
}

TEST_CASE("The last predator meal takes only the meat remaining after a hunter harvest", "[AIWorld][LivingSupply]")
{
    auto ledger = SupplyFixture();
    Harvest(ledger, 101, {300, 1}, 4);
    auto meal = ledger.Reserve(201, {300, 1}, ClaimKind::PredatorMeal, 3, 6000);
    REQUIRE(meal);
    REQUIRE(ledger.GetCarcass({300, 1})->Reserved == 2);
    auto result = ledger.Commit(*meal, 11000);
    REQUIRE(result.Success);
    REQUIRE(result.Portions == 2);
    REQUIRE(ledger.GetCounters().PredatorConsumed == 2);
    REQUIRE(ledger.GetCarcass({300, 1})->Remaining == 0);
    REQUIRE_FALSE(ledger.Reserve(202, {300, 1}, ClaimKind::PredatorMeal, 3, 11001));
    Conserved(ledger);
}

TEST_CASE("A new prey life cancels old-life reservations even while the old corpse was active", "[AIWorld][LivingSupply]")
{
    auto ledger = SupplyFixture();
    REQUIRE(ledger.RegisterCarcass({300, 1}, 1, 20000));
    auto oldLife = ledger.Reserve(101, {300, 1}, ClaimKind::Harvest, 6, 2);
    REQUIRE(oldLife);
    REQUIRE(ledger.RegisterCarcass({300, 2}, 3, 30000));
    REQUIRE_FALSE(ledger.Commit(*oldLife, 4).Success);
    REQUIRE(ledger.GetCargo(101) == 0);
    REQUIRE(ledger.GetCarcass({300, 2})->Remaining == 6);
    REQUIRE(ledger.GetCarcass({300, 2})->Reserved == 0);
    REQUIRE(ledger.GetCounters().PotentialLost == 6);
    auto current = ledger.Reserve(101, {300, 2}, ClaimKind::Harvest, 6, 5);
    REQUIRE(current);
    REQUIRE(ledger.Commit(*current, 5005).Success);
    REQUIRE(ledger.GetCounters().HarvestedTotal == 6);
    Conserved(ledger);
}

TEST_CASE("An interrupted or expired corpse claim releases food for another actor", "[AIWorld][LivingSupply]")
{
    auto ledger = SupplyFixture();
    CarcassKey key{300, 1};
    REQUIRE(ledger.RegisterCarcass(key, 1, 1000));
    auto first = ledger.Reserve(101, key, ClaimKind::Harvest, 6, 2, 10);
    REQUIRE(first);
    SECTION("Explicit interruption")
    {
        REQUIRE(ledger.Cancel(*first));
        REQUIRE_FALSE(ledger.Cancel(*first));
        REQUIRE_FALSE(ledger.Commit(*first, 4).Success);
    }
    SECTION("Expiry at the exact deadline")
    {
        ledger.Expire(12);
        REQUIRE_FALSE(ledger.Commit(*first, 12).Success);
    }
    REQUIRE(ledger.GetCarcass(key)->Remaining == 6);
    REQUIRE(ledger.GetCarcass(key)->Reserved == 0);
    REQUIRE(ledger.GetCargo(101) == 0);
    auto next = ledger.Reserve(102, key, ClaimKind::Harvest, 6, 13);
    REQUIRE(next);
    REQUIRE(ledger.Commit(*next, 14).Success);
    REQUIRE(ledger.GetCargo(102) == 6);
    Conserved(ledger);
}

TEST_CASE("An actor has one owned reservation and cannot steal another actor token", "[AIWorld][LivingSupply]")
{
    auto ledger = SupplyFixture();
    REQUIRE(ledger.RegisterCarcass({300, 1}, 1, 20000));
    REQUIRE(ledger.RegisterCarcass({301, 1}, 1, 20000));
    auto first = ledger.Reserve(101, {300, 1}, ClaimKind::Harvest, 3, 2);
    REQUIRE(first);
    auto repeat = ledger.Reserve(101, {300, 1}, ClaimKind::Harvest, 3, 3);
    REQUIRE(repeat);
    REQUIRE(*first == *repeat);
    REQUIRE(ledger.GetCarcass({300, 1})->Reserved == 3);
    REQUIRE_FALSE(ledger.Reserve(101, {300, 1}, ClaimKind::Harvest, 6, 3));
    REQUIRE_FALSE(ledger.Reserve(101, {301, 1}, ClaimKind::Harvest, 3, 3));
    REQUIRE_FALSE(ledger.Commit({102, first->Serial}, 4).Success);
    REQUIRE_FALSE(ledger.Cancel({102, first->Serial}));
    REQUIRE(ledger.GetCarcass({300, 1})->Reserved == 3);
    REQUIRE(ledger.CancelActor(101));
    REQUIRE_FALSE(ledger.CancelActor(101));
    REQUIRE(ledger.GetCarcass({300, 1})->Reserved == 0);
    Conserved(ledger);
}

TEST_CASE("Despawn or corpse expiry cancels outstanding harvest without creating cargo", "[AIWorld][LivingSupply]")
{
    auto ledger = SupplyFixture();
    CarcassKey key{300, 1};
    REQUIRE(ledger.RegisterCarcass(key, 1, 100));
    auto token = ledger.Reserve(101, key, ClaimKind::Harvest, 6, 2);
    REQUIRE(token);
    SECTION("Corpse disappears") { ledger.CloseCarcass(key); }
    SECTION("Corpse reaches its own deadline") { ledger.Expire(100); }
    REQUIRE_FALSE(ledger.GetCarcass(key)->Active);
    REQUIRE(ledger.GetCarcass(key)->Remaining == 0);
    REQUIRE(ledger.GetCarcass(key)->Reserved == 0);
    REQUIRE(ledger.GetCounters().PotentialLost == 6);
    REQUIRE(ledger.GetCounters().HarvestedTotal == 0);
    REQUIRE_FALSE(ledger.Commit(*token, 101).Success);
    REQUIRE(ledger.GetCargo(101) == 0);
    ledger.CloseCarcass(key);
    REQUIRE(ledger.GetCounters().PotentialLost == 6);
    Conserved(ledger);
}

TEST_CASE("A repeated death observation cannot recreate meat and a genuine new life can", "[AIWorld][LivingSupply]")
{
    auto ledger = SupplyFixture();
    REQUIRE(ledger.RegisterCarcass({300, 10}, 1, 1000));
    REQUIRE_FALSE(ledger.RegisterCarcass({300, 10}, 2, 2000));
    REQUIRE_FALSE(ledger.RegisterCarcass({300, 9}, 2, 2000));
    auto token = ledger.Reserve(101, {300, 10}, ClaimKind::Harvest, 3, 3);
    REQUIRE(token);
    REQUIRE(ledger.Commit(*token, 4).Success);
    REQUIRE(ledger.RegisterCarcass({300, 11}, 5, 2000));
    REQUIRE(ledger.GetCarcass({300, 10}) == nullptr);
    REQUIRE(ledger.GetCarcass({300, 11})->Remaining == 6);
    REQUIRE(ledger.GetCounters().PotentialCreated == 12);
    REQUIRE(ledger.GetCounters().PotentialLost == 3);
    REQUIRE_FALSE(ledger.RegisterCarcass({300, 10}, 6, 3000));
    ledger.CloseCarcass({300, 11});
    REQUIRE_FALSE(ledger.RegisterCarcass({300, 11}, 7, 3000));
    Conserved(ledger);
}

TEST_CASE("A full depot preserves cargo and a partial delivery transfers only free capacity", "[AIWorld][LivingSupply]")
{
    auto ledger = SupplyFixture(28);
    Harvest(ledger, 101, {300, 1});
    auto delivery = ledger.Deliver(101, 10, 1, 6, true);
    REQUIRE(delivery.Success);
    REQUIRE(delivery.Portions == 2);
    REQUIRE(ledger.GetCargo(101) == 4);
    REQUIRE(ledger.GetDepot(1)->Stock == 30);
    REQUIRE_FALSE(ledger.Deliver(101, 11, 1, 6, true).Success);
    REQUIRE(ledger.GetCargo(101) == 4);
    REQUIRE(ledger.EatDepot(101, 11, 1, 3, true).Success);
    REQUIRE(ledger.Deliver(101, 12, 1, 6, true).Portions == 3);
    REQUIRE(ledger.GetCargo(101) == 1);
    REQUIRE(ledger.GetDepot(1)->Stock == 30);
    Conserved(ledger);
}

TEST_CASE("Hunters cannot harvest beyond carrying capacity or take meals from another camp", "[AIWorld][LivingSupply]")
{
    auto ledger = SupplyFixture(2);
    Harvest(ledger, 101, {300, 1}, 4);
    REQUIRE(ledger.RegisterCarcass({301, 1}, 1, 20000));
    auto token = ledger.Reserve(101, {301, 1}, ClaimKind::Harvest, 6, 2);
    REQUIRE(token);
    REQUIRE(ledger.Commit(*token, 3).Portions == 2);
    REQUIRE(ledger.GetCargo(101) == 6);
    REQUIRE_FALSE(ledger.Reserve(101, {301, 1}, ClaimKind::Harvest, 1, 4));
    REQUIRE_FALSE(ledger.Deliver(101, 1, 2, 6, true).Success);
    REQUIRE_FALSE(ledger.EatDepot(102, 1, 1, 1, true).Success);
    REQUIRE_FALSE(ledger.EatDepot(101, 1, 1, 3, true).Success);
    REQUIRE(ledger.GetDepot(1)->Stock == 2);
    REQUIRE(ledger.GetCargo(101) == 6);
    Conserved(ledger);
}

TEST_CASE("Actor death accounts lost cargo and invalidates previous-life receipts", "[AIWorld][LivingSupply]")
{
    auto ledger = SupplyFixture();
    Harvest(ledger, 101, {300, 1});
    REQUIRE(ledger.Deliver(101, 10, 1, 2, true).Success);
    REQUIRE(ledger.RegisterCarcass({301, 1}, 1, 20000));
    auto pending = ledger.Reserve(101, {301, 1}, ClaimKind::Harvest, 2, 2);
    REQUIRE(pending);
    REQUIRE(ledger.DropCargo(101) == 4);
    REQUIRE(ledger.GetCargo(101) == 0);
    REQUIRE(ledger.GetCarcass({301, 1})->Reserved == 0);
    REQUIRE_FALSE(ledger.Commit(*pending, 3).Success);
    REQUIRE_FALSE(ledger.Deliver(101, 10, 1, 2, true).Success);
    REQUIRE(ledger.GetCounters().LostTotal == 4);
    REQUIRE(ledger.DropCargo(101) == 0);
    REQUIRE(ledger.GetCounters().LostTotal == 4);
    Conserved(ledger);
}

TEST_CASE("Supply registration and operations reject foreign or invalid participants", "[AIWorld][LivingSupply]")
{
    Ledger ledger;
    REQUIRE_FALSE(ledger.ConfigureDepot(0));
    REQUIRE_FALSE(ledger.ConfigureDepot(1, 31));
    REQUIRE_FALSE(ledger.ConfigureDepot(1, 0, 3, 4, 2));
    REQUIRE(ledger.ConfigureDepot(1, 2));
    REQUIRE(ledger.ConfigureDepot(1, 2));
    REQUIRE(ledger.GetCounters().InitialFood == 2);
    REQUIRE_FALSE(ledger.ConfigureDepot(1, 3));
    REQUIRE(ledger.ConfigureDepot(2));
    REQUIRE(ledger.ConfigureDepot(3));
    REQUIRE(ledger.ConfigureDepot(4));
    REQUIRE(ledger.ConfigureDepot(5));
    REQUIRE(ledger.ConfigureDepot(6));
    REQUIRE_FALSE(ledger.ConfigureDepot(7));
    REQUIRE_FALSE(ledger.RegisterActor(0, 1));
    REQUIRE_FALSE(ledger.RegisterActor(101, 7));
    REQUIRE_FALSE(ledger.RegisterActor(101, 1, 7));
    REQUIRE_FALSE(ledger.RegisterActor(201, 1, 0, true));
    REQUIRE_FALSE(ledger.RegisterActor(201, 0, 6, true));
    REQUIRE(ledger.RegisterActor(101, 1));
    REQUIRE(ledger.RegisterActor(101, 1));
    REQUIRE_FALSE(ledger.RegisterActor(101, 2));
    REQUIRE(ledger.RegisterActor(201, 0, 0, true));
    REQUIRE_FALSE(ledger.RegisterCarcass({0, 1}, 1, 100));
    REQUIRE_FALSE(ledger.RegisterCarcass({300, 0}, 1, 100));
    REQUIRE_FALSE(ledger.RegisterCarcass({300, 1}, 100, 100));
    REQUIRE_FALSE(ledger.RegisterCarcass({300, 1}, 1, 100, 0));
    REQUIRE_FALSE(ledger.RegisterCarcass({300, 1}, 1, 100, 7));
    REQUIRE(ledger.RegisterCarcass({300, 1}, 1, 100));
    REQUIRE_FALSE(ledger.Reserve(999, {300, 1}, ClaimKind::Harvest, 6, 2));
    REQUIRE_FALSE(ledger.Reserve(101, {300, 1}, ClaimKind::PredatorMeal, 3, 2));
    REQUIRE_FALSE(ledger.Reserve(201, {300, 1}, ClaimKind::Harvest, 3, 2));
    REQUIRE_FALSE(ledger.Reserve(201, {300, 1}, ClaimKind::PredatorMeal, 4, 2));
    REQUIRE_FALSE(ledger.Reserve(101, {300, 1}, ClaimKind::Harvest, 0, 2));
    REQUIRE_FALSE(ledger.Reserve(101, {300, 1}, ClaimKind::Harvest, 3, 2, 0));
    REQUIRE_FALSE(ledger.Reserve(101, {300, 1}, ClaimKind(255), 3, 2));
    REQUIRE_FALSE(ledger.Deliver(999, 1, 1, 1, true).Success);
    REQUIRE_FALSE(ledger.EatDepot(999, 1, 1, 1, true).Success);
    REQUIRE_FALSE(ledger.Deliver(101, 0, 1, 1, true).Success);
    REQUIRE_FALSE(ledger.EatDepot(201, 1, 1, 1, true).Success);
    REQUIRE(ledger.GetCargo(999) == 0);
    REQUIRE(ledger.GetDepot(7) == nullptr);
    REQUIRE(ledger.GetCarcass({999, 1}) == nullptr);
    Conserved(ledger);
}

TEST_CASE("Expired commit cannot mint food even when the corpse remains", "[AIWorld][LivingSupply]")
{
    auto ledger = SupplyFixture();
    REQUIRE(ledger.RegisterCarcass({300, 1}, 1, 100000));
    auto token = ledger.Reserve(101, {300, 1}, ClaimKind::Harvest, 6, 2, 5);
    REQUIRE(token);
    REQUIRE_FALSE(ledger.Commit(*token, 7).Success);
    REQUIRE(ledger.GetCargo(101) == 0);
    REQUIRE(ledger.GetCarcass({300, 1})->Remaining == 6);
    REQUIRE(ledger.GetCarcass({300, 1})->Reserved == 0);
    REQUIRE(ledger.GetCounters().HarvestedTotal == 0);
    Conserved(ledger);
}

TEST_CASE("Supply search preserves one pending waypoint across repeated planning budget yields", "[AIWorld][LivingSupply]")
{
    LivingSupply::ActorRuntime actor;
    ActionPosition ground{725, 245, 1020, 10.5f};
    ActionPosition first = LivingSupply::PrepareHunterSearch(actor, ground);
    REQUIRE(actor.PendingSearch.has_value());
    REQUIRE(actor.PendingSearchCount);
    for (unsigned turn = 0; turn < 20; ++turn)
    {
        LivingSupply::FinishHunterSearchAttempt(actor, false);
        auto retry = LivingSupply::PrepareHunterSearch(actor, ground);
        REQUIRE(retry.MapId == first.MapId);
        REQUIRE(retry.X == first.X);
        REQUIRE(retry.Y == first.Y);
        REQUIRE(retry.Z == first.Z);
        REQUIRE(actor.EmptySearches == 0);
        REQUIRE(actor.SearchLeg == 0);
    }
    LivingSupply::FinishHunterSearchAttempt(actor, true);
    REQUIRE(actor.EmptySearches == 1);
    REQUIRE(actor.SearchLeg == 1);
    REQUIRE_FALSE(actor.PendingSearch.has_value());
    REQUIRE_FALSE(actor.PendingSearchCount);
    LivingSupply::FinishHunterSearchAttempt(actor, true);
    REQUIRE(actor.EmptySearches == 1);
    auto next = LivingSupply::PrepareHunterSearch(actor, ground);
    REQUIRE(next.X != first.X);
    REQUIRE(next.Y != first.Y);
    REQUIRE(std::hypot(next.X - ground.X, next.Y - ground.Y) <= 36.001f);
    LivingSupply::ClearHunterSearch(actor);
    REQUIRE_FALSE(actor.PendingSearch.has_value());
    REQUIRE_FALSE(actor.PendingSearchCount);
    REQUIRE(actor.EmptySearches == 1);
}

TEST_CASE("Supply search counts eight real attempts independently of denied turns", "[AIWorld][LivingSupply]")
{
    LivingSupply::ActorRuntime actor;
    ActionPosition ground{725, 245, 1020, 10.5f};
    for (uint32 attempt = 0; attempt < 8; ++attempt)
    {
        auto point = LivingSupply::PrepareHunterSearch(actor, ground);
        REQUIRE(std::hypot(point.X - ground.X, point.Y - ground.Y) <= 36.001f);
        LivingSupply::FinishHunterSearchAttempt(actor, false);
        REQUIRE(actor.EmptySearches == attempt);
        LivingSupply::FinishHunterSearchAttempt(actor, true);
        REQUIRE(actor.EmptySearches == attempt + 1);
        REQUIRE(actor.SearchLeg == attempt + 1);
    }
    REQUIRE(actor.EmptySearches == 8);
    REQUIRE_FALSE(actor.PendingSearch.has_value());
}

TEST_CASE("Depot consumers require an explicit zero-cargo capability and cannot become harvesters", "[AIWorld][LivingSupply][Consumers]")
{
    auto ledger = SupplyFixture();
    REQUIRE_FALSE(ledger.RegisterActor(301, 1, 0));
    REQUIRE_FALSE(ledger.RegisterActor(301, 3, 0, false, true));
    REQUIRE_FALSE(ledger.RegisterActor(301, 1, 6, false, true));
    REQUIRE_FALSE(ledger.RegisterActor(301, 0, 0, true, true));
    REQUIRE(ledger.RegisterActor(301, 1, 0, false, true));
    REQUIRE(ledger.RegisterActor(301, 1, 0, false, true));
    REQUIRE_FALSE(ledger.RegisterActor(301, 1, 6));
    REQUIRE_FALSE(ledger.RegisterActor(301, 2, 0, false, true));
    REQUIRE(ledger.RegisterCarcass({401, 1}, 1, 30000));
    REQUIRE_FALSE(ledger.Reserve(301, {401, 1}, ClaimKind::Harvest, 1, 2));
    REQUIRE_FALSE(ledger.Reserve(301, {401, 1}, ClaimKind::PredatorMeal, 1, 2));
    REQUIRE_FALSE(ledger.Deliver(301, 1, 1, 1, true).Success);
    REQUIRE_FALSE(ledger.Commit({301, 1}, 3).Success);
    REQUIRE(ledger.GetCargo(301) == 0);
    REQUIRE(ledger.GetCarcass({401, 1})->Remaining == 6);
    REQUIRE(ledger.GetCounters().HarvestedTotal == 0);
    Conserved(ledger);
}

TEST_CASE("Two consumers compete atomically for the final depot portion", "[AIWorld][LivingSupply][Consumers]")
{
    auto ledger = SupplyFixture(1);
    REQUIRE(ledger.RegisterActor(301, 1, 0, false, true));
    REQUIRE(ledger.RegisterActor(302, 1, 0, false, true));
    auto first = ledger.EatDepot(301, 1, 1, 1, true);
    REQUIRE(first.Success);
    REQUIRE_FALSE(first.Duplicate);
    REQUIRE(first.Portions == 1);
    auto other = ledger.EatDepot(302, 1, 1, 1, true);
    REQUIRE_FALSE(other.Success);
    REQUIRE(other.Portions == 0);
    REQUIRE(std::string(other.Reason) == "NO_FOOD");
    REQUIRE(ledger.GetDepot(1)->Stock == 0);
    REQUIRE(ledger.GetCounters().DepotConsumed == 1);
    REQUIRE(ledger.GetCounters().ConsumedTotal == 1);
    auto replay = ledger.EatDepot(301, 1, 1, 1, true);
    REQUIRE(replay.Success);
    REQUIRE(replay.Duplicate);
    REQUIRE(ledger.GetCounters().ConsumedTotal == 1);
    Conserved(ledger);
}

TEST_CASE("A consumer at an empty or interrupted depot meal receives no successful receipt", "[AIWorld][LivingSupply][Consumers]")
{
    auto ledger = SupplyFixture(1);
    REQUIRE(ledger.RegisterActor(301, 1, 0, false, true));
    REQUIRE_FALSE(ledger.EatDepot(301, 1, 1, 1, false).Success);
    REQUIRE(ledger.GetDepot(1)->Stock == 1);
    REQUIRE(ledger.GetCounters().ConsumedTotal == 0);
    REQUIRE_FALSE(ledger.EatDepot(301, 1, 2, 1, true).Success);
    REQUIRE(ledger.EatDepot(301, 1, 1, 1, true).Success);
    REQUIRE_FALSE(ledger.EatDepot(301, 2, 1, 1, true).Success);
    REQUIRE_FALSE(ledger.EatDepot(301, 3, 1, 1, true).Success);
    REQUIRE(ledger.GetCounters().ConsumedTotal == 1);
    REQUIRE(ledger.GetCargo(301) == 0);
    Conserved(ledger);
}

TEST_CASE("Consumer meals debit food actually delivered by a hunter without granting cargo", "[AIWorld][LivingSupply][Consumers]")
{
    auto ledger = SupplyFixture();
    REQUIRE(ledger.RegisterActor(301, 1, 0, false, true));
    REQUIRE_FALSE(ledger.EatDepot(301, 1, 1, 1, true).Success);
    Harvest(ledger, 101, {401, 1});
    REQUIRE(ledger.Deliver(101, 1, 1, 6, true).Success);
    REQUIRE(ledger.GetDepot(1)->Stock == 6);
    REQUIRE(ledger.EatDepot(301, 1, 1, 1, true).Success);
    REQUIRE(ledger.GetDepot(1)->Stock == 5);
    REQUIRE(ledger.GetCargo(301) == 0);
    REQUIRE(ledger.GetCounters().HarvestedTotal == 6);
    REQUIRE(ledger.GetCounters().DeliveredTotal == 6);
    REQUIRE(ledger.GetCounters().ConsumedTotal == 1);
    REQUIRE(ledger.ConfigureDepot(1));
    REQUIRE(ledger.GetDepot(1)->Stock == 5);
    Conserved(ledger);
}

TEST_CASE("Only the reviewed two workers and gnoll gatherer acquire depot-consumer capability", "[AIWorld][LivingSupply][Consumers]")
{
    for (auto const& spec : LivingSupply::Consumers)
    {
        REQUIRE(LivingSupply::ConsumerCapability(spec.Spawn, 725, spec.Type, AgentControlMode::AIWorldControlled, true));
        REQUIRE_FALSE(LivingSupply::ConsumerCapability(spec.Spawn, 725, spec.Type, AgentControlMode::ObserveOnly, true));
        REQUIRE_FALSE(LivingSupply::ConsumerCapability(spec.Spawn, 0, spec.Type, AgentControlMode::AIWorldControlled, true));
        REQUIRE_FALSE(LivingSupply::ConsumerCapability(spec.Spawn, 725, spec.Type, AgentControlMode::AIWorldControlled, false));
        REQUIRE_FALSE(LivingSupply::ConsumerCapability(spec.Spawn, 725, AgentType::Predator, AgentControlMode::AIWorldControlled, true));
        REQUIRE_FALSE(LivingSupply::HunterCapability(spec.Spawn, 725, AgentType::Combatant, AgentControlMode::AIWorldControlled, true));
    }
    REQUIRE_FALSE(LivingSupply::ConsumerCapability(901063, 725, AgentType::Civilian, AgentControlMode::AIWorldControlled, true));
    REQUIRE_FALSE(LivingSupply::ConsumerCapability(901035, 725, AgentType::Combatant, AgentControlMode::AIWorldControlled, true));
    REQUIRE_FALSE(LivingSupply::ConsumerCapability(901034, 725, AgentType::Combatant, AgentControlMode::AIWorldControlled, true));
    REQUIRE_FALSE(LivingSupply::ConsumerCapability(901037, 725, AgentType::Civilian, AgentControlMode::AIWorldControlled, true));
    REQUIRE(LivingSupply::Consumer(901035)->Depot == 1);
    REQUIRE(LivingSupply::Consumer(901036)->Depot == 1);
    REQUIRE(LivingSupply::Consumer(901063)->Depot == 2);
}

TEST_CASE("Supply planning preserves an issued corridor and candidate cursor across budget yields", "[AIWorld][LivingSupply][SupplyPlanning]")
{
    LivingSupply::TravelContext travel;
    ActionPosition from{725, 0, 0, 0}, goal{725, 30, 0, 0};
    travel.Begin({}, from, goal, from, std::nullopt, 8, 1, 1);
    travel.Search.Begin(from, goal, 3, 384, nullptr, 8, true);
    travel.Search.IssueCorridor(false);
    travel.Route.Planned = {{725, 10, 2, 0}, goal};
    for (unsigned denied = 0; denied < 30; ++denied)
    {
        REQUIRE(travel.ObserveSource(from));
        REQUIRE(travel.Search.NextTarget == 1);
        REQUIRE(travel.Route.Planned.size() == 2);
        REQUIRE(travel.Route.Planned.front().X == 10);
        REQUIRE_FALSE(travel.Route.Failed(from, travel.Route.Planned.front()));
    }
    auto first = travel.Route.Planned.front();
    travel.RejectCandidate(from, first);
    REQUIRE(travel.Search.NextTarget == 1);
    REQUIRE(travel.Route.Planned.empty());
    REQUIRE(travel.Route.Failed(from, first));
    REQUIRE_FALSE(travel.Route.Failed(from, {725, 10, -2, 0}));
    travel.Search.IssueCorridor(false);
    REQUIRE(travel.Search.NextTarget == 2);
    REQUIRE(travel.Route.Failed(from, first));
}

TEST_CASE("Only an owned reached supply leg consumes one corner and preserves the surface proof", "[AIWorld][LivingSupply][SupplyPlanning]")
{
    LivingSupply::TravelContext travel;
    ActionPosition from{725, 0, 0, 0}, corner{725, 2.5f, 0, 0}, next{725, 2.5f, 1.25f, 0}, goal{725, 6, 2, 0};
    travel.Begin({}, from, goal, from, std::nullopt, 8, 1, 1);
    travel.Route.Planned = {corner, next, goal};
    travel.Route.SurfaceCorridor = true;
    RecoveryMovement request{corner, from, 384, std::nullopt, false};
    request.SurfaceCorridor = true;
    request.QueryDestination = corner;
    // Its physical endpoint was normalized by the independent strict builder.
    request.Destination.Z = 0.2f;
    travel.Pending = request;
    travel.PendingFrom = from;
    REQUIRE_FALSE(travel.ObserveSource(request.Destination));
    REQUIRE(travel.Route.Planned.size() == 3);
    REQUIRE(travel.Pending.has_value());
    REQUIRE(travel.ObserveSource(request.Destination, true));
    REQUIRE_FALSE(travel.Pending.has_value());
    REQUIRE(travel.Route.Planned.size() == 2);
    REQUIRE(travel.Route.Planned.front().Y == 1.25f);
    REQUIRE(travel.Route.SurfaceCorridor);
    REQUIRE(travel.ObserveSource(request.Destination));
    REQUIRE(travel.Route.Planned.size() == 2);
    REQUIRE_FALSE(travel.ObserveSource({725, 3.2f, 0, 0}));
    // Being within the ordinary three-yard arrival radius never discards the
    // next protected bend, nor does a second arrival without an issued leg.
    REQUIRE_FALSE(travel.ObserveSource(next, true));
    REQUIRE(travel.Route.Planned.size() == 2);
}

TEST_CASE("Supply route identity rejects changed goal lifecycle danger phase and source contexts", "[AIWorld][LivingSupply][SupplyPlanning]")
{
    LivingSupply::TravelContext travel;
    ActionPosition from{725, 0, 0, 0}, goal{725, 20, 0, 0}, home{725, -5, 0, 0}, danger{725, 10, 10, 0};
    auto guid = ObjectGuid::Create<HighGuid::Unit>(1000, 1);
    travel.Begin(guid, from, goal, home, danger, 8, 1, 1);
    REQUIRE(travel.Matches(guid, goal, home, danger, 8, 1, 1));
    REQUIRE_FALSE(travel.Matches(ObjectGuid::Create<HighGuid::Unit>(1000, 2), goal, home, danger, 8, 1, 1));
    REQUIRE_FALSE(travel.Matches(guid, {725, 21, 0, 0}, home, danger, 8, 1, 1));
    REQUIRE_FALSE(travel.Matches(guid, goal, {725, -6, 0, 0}, danger, 8, 1, 1));
    REQUIRE_FALSE(travel.Matches(guid, goal, home, std::nullopt, 8, 1, 1));
    REQUIRE_FALSE(travel.Matches(guid, goal, home, ActionPosition{725, 10, 11, 0}, 8, 1, 1));
    REQUIRE_FALSE(travel.Matches(guid, goal, home, danger, 9, 1, 1));
    REQUIRE_FALSE(travel.Matches(guid, goal, home, danger, 8, 2, 1));
    REQUIRE_FALSE(travel.Matches(guid, goal, home, danger, 8, 1, 3));
    REQUIRE_FALSE(travel.ObserveSource({726, 0, 0, 0}));
    REQUIRE_FALSE(travel.ObserveSource({725, 0.1f, 0, 0}));
    travel.Search.NextTarget = 5;
    travel.Route.MarkIneffective(from, goal);
    travel.Begin(guid, from, {725, 21, 0, 0}, home, danger, 8, 1, 1);
    REQUIRE(travel.Search.NextTarget == 0);
    REQUIRE(travel.Route.FailedEdges.empty());
    travel = {};
    REQUIRE_FALSE(travel.Active);
    REQUIRE_FALSE(travel.ObserveSource(from));
}

TEST_CASE("Supply's live blocked-source fixtures retain the first bend before executing a later endpoint", "[AIWorld][LivingSupply][SupplyPlanning]")
{
    using LivingReturnPolicy::Distance;
    // Actual 901035 and 901063 mesh corridors from the failing live run. A
    // 28-yard arc-length endpoint lies beyond the bend; strict short legs must
    // reach its preceding corridor corner before issuing the following one.
    std::vector<std::vector<ActionPosition>> paths{
        {{725, -406.6194f, 495.3748f, 0}, {725, -391.2f, 490.9332f, 0.21656f}, {725, -372.4612f, 492.6822f, 0.3998f}},
        {{725, 483.4182f, 981.9759f, 15.1422f}, {725, 465.8665f, 977.8665f, 14.8832f}, {725, 463.9977f, 972.5791f, 13.2545f}}
    };
    for (auto const& path : paths)
    {
        auto corridor = LivingReturnPolicy::Corridor(path);
        REQUIRE(corridor.size() >= 3);
        REQUIRE(Distance(path.front(), corridor.front()) <= 14.001f);
        REQUIRE(Distance(path.front(), corridor.front()) < Distance(path.front(), path[1]));
        REQUIRE(std::any_of(corridor.begin(), corridor.end(), [&](ActionPosition const& point)
            { return Distance(point, path[1]) < 0.01f; }));
        auto old = LivingReturnPolicy::PathStep(path);
        REQUIRE(old.has_value());
        REQUIRE(Distance(path.front(), *old) > Distance(path.front(), path[1]));
        LivingSupply::TravelContext travel;
        travel.Begin({}, path.front(), path.back(), path.front(), std::nullopt, 8, 1, 1);
        travel.Route.Planned = corridor;
        auto first = corridor.front();
        travel.Pending = RecoveryMovement{first, path.front(), 384, std::nullopt, false};
        travel.PendingFrom = path.front();
        REQUIRE(travel.ObserveSource(first, true));
        REQUIRE(travel.Route.Planned.size() == corridor.size() - 1);
        REQUIRE(Distance(travel.Route.Planned.front(), path[1]) < 0.01f);
    }
}

TEST_CASE("Rejected or budget-suspended supply travel cannot complete a consumer meal", "[AIWorld][LivingSupply][SupplyPlanning]")
{
    auto ledger = SupplyFixture(1);
    REQUIRE(ledger.RegisterActor(301, 1, 0, false, true));
    LivingSupply::TravelContext travel;
    ActionPosition from{725, 0, 0, 0}, goal{725, 20, 0, 0};
    travel.Begin({}, from, goal, from, std::nullopt, 8, 1, 1);
    travel.Route.Planned = {{725, 10, 0, 0}, goal};
    for (unsigned denied = 0; denied < 10; ++denied)
        REQUIRE(travel.ObserveSource(from));
    travel.RejectCandidate(from, travel.Route.Planned.front());
    REQUIRE_FALSE(ledger.EatDepot(301, 1, 1, 1, false).Success);
    REQUIRE(ledger.GetDepot(1)->Stock == 1);
    REQUIRE(ledger.GetCounters().ConsumedTotal == 0);
    REQUIRE(ledger.GetCargo(301) == 0);
    Conserved(ledger);
}

TEST_CASE("Raider theft requires a separate exact source home and bounded cargo capability", "[AIWorld][LivingSupply][Raiders]")
{
    auto ledger = SupplyFixture(6);
    REQUIRE_FALSE(ledger.RegisterRaider(301)); // Its home warehouse is absent.
    REQUIRE(ledger.ConfigureDepot(3));
    REQUIRE(ledger.ConfigureDepot(4));
    REQUIRE_FALSE(ledger.RegisterRaider(0));
    REQUIRE_FALSE(ledger.RegisterRaider(301, 2, 1));
    REQUIRE_FALSE(ledger.RegisterRaider(301, 3, 2));
    REQUIRE_FALSE(ledger.RegisterRaider(301, 3, 1, 0));
    REQUIRE_FALSE(ledger.RegisterRaider(301, 3, 1, 1));
    REQUIRE_FALSE(ledger.RegisterRaider(301, 3, 1, 3));
    REQUIRE_FALSE(ledger.RegisterRaider(101));
    REQUIRE_FALSE(ledger.RegisterRaider(201));
    REQUIRE(ledger.RegisterActor(302, 3, 2));
    REQUIRE_FALSE(ledger.RegisterRaider(302)); // Identical cargo is no authority upgrade.
    REQUIRE(ledger.RegisterRaider(301));
    REQUIRE(ledger.RegisterRaider(301));
    REQUIRE_FALSE(ledger.RegisterActor(301, 3, 2));
    REQUIRE_FALSE(ledger.RegisterActor(301, 3, 0, false, true));
    REQUIRE_FALSE(ledger.RegisterActor(301, 0, 0, true));
    REQUIRE(ledger.RegisterCarcass({401, 1}, 1, 30000));
    REQUIRE_FALSE(ledger.Reserve(301, {401, 1}, ClaimKind::Harvest, 2, 2));
    REQUIRE_FALSE(ledger.Reserve(301, {401, 1}, ClaimKind::PredatorMeal, 2, 2));
    REQUIRE_FALSE(ledger.Commit({301, 1}, 3).Success);
    REQUIRE(ledger.GetCarcass({401, 1})->Remaining == 6);
    REQUIRE(ledger.RegisterActor(303, 1, 0, false, true));
    for (uint64 forbidden : {101ull, 201ull, 302ull, 303ull, 999ull})
        REQUIRE_FALSE(ledger.Steal(forbidden, 1, 1, 2, true).Success);
    REQUIRE(ledger.GetDepot(1)->Stock == 6);
    REQUIRE(ledger.GetCounters().StolenTotal == 0);
    Conserved(ledger);
}

TEST_CASE("Only completed theft transfers stock and its receipt shares the delivery and meal watermark", "[AIWorld][LivingSupply][Raiders]")
{
    auto ledger = RaidFixture();
    REQUIRE_FALSE(ledger.Steal(301, 1, 1, 2, false).Success);
    REQUIRE_FALSE(ledger.Steal(301, 0, 1, 2, true).Success);
    REQUIRE_FALSE(ledger.Steal(301, 1, 2, 2, true).Success);
    REQUIRE_FALSE(ledger.Steal(301, 1, 3, 2, true).Success);
    REQUIRE_FALSE(ledger.Steal(301, 1, 4, 2, true).Success);
    REQUIRE_FALSE(ledger.Steal(301, 1, 1, 0, true).Success);
    REQUIRE_FALSE(ledger.Steal(301, 1, 1, 3, true).Success);
    REQUIRE(ledger.GetDepot(1)->Stock == 6);
    REQUIRE(ledger.GetCargo(301) == 0);
    auto stolen = ledger.Steal(301, 1, 1, 2, true);
    REQUIRE(stolen.Success);
    REQUIRE_FALSE(stolen.Duplicate);
    REQUIRE(stolen.Portions == 2);
    REQUIRE(ledger.GetCargo(301) == 2);
    REQUIRE(ledger.GetDepot(1)->Stock == 4);
    REQUIRE(ledger.GetCounters().StolenTotal == 2);
    REQUIRE(ledger.GetCounters().HarvestedTotal == 0);
    REQUIRE(ledger.GetCounters().ConsumedTotal == 0);
    Conserved(ledger);
    auto replay = ledger.Steal(301, 1, 1, 2, true);
    REQUIRE(replay.Success);
    REQUIRE(replay.Duplicate);
    REQUIRE_FALSE(ledger.Steal(301, 1, 1, 1, true).Success);
    REQUIRE_FALSE(ledger.Steal(301, 1, 2, 2, true).Success);
    REQUIRE_FALSE(ledger.Deliver(301, 1, 3, 2, true).Success);
    REQUIRE_FALSE(ledger.EatDepot(301, 1, 3, 1, true).Success);
    REQUIRE_FALSE(ledger.Deliver(301, 2, 1, 2, true).Success);
    REQUIRE_FALSE(ledger.EatDepot(301, 2, 1, 1, true).Success);
    REQUIRE_FALSE(ledger.Deliver(301, 2, 3, 2, false).Success);
    REQUIRE(ledger.GetCargo(301) == 2);
    REQUIRE(ledger.Deliver(301, 2, 3, 2, true).Success);
    REQUIRE(ledger.GetCargo(301) == 0);
    REQUIRE(ledger.GetDepot(3)->Stock == 2);
    REQUIRE_FALSE(ledger.Steal(301, 1, 1, 2, true).Success);
    REQUIRE_FALSE(ledger.Steal(301, 2, 1, 2, true).Success);
    REQUIRE_FALSE(ledger.EatDepot(301, 2, 3, 1, true).Success);
    REQUIRE(ledger.EatDepot(301, 3, 3, 1, true).Success);
    REQUIRE(ledger.EatDepot(301, 3, 3, 1, true).Duplicate);
    REQUIRE_FALSE(ledger.Steal(301, 3, 1, 1, true).Success);
    REQUIRE_FALSE(ledger.Deliver(301, 3, 3, 1, true).Success);
    REQUIRE(ledger.GetDepot(3)->Stock == 1);
    REQUIRE(ledger.GetCounters().StolenTotal == 2);
    REQUIRE(ledger.GetCounters().DeliveredTotal == 2);
    REQUIRE(ledger.GetCounters().ConsumedTotal == 1);
    Conserved(ledger);
}

TEST_CASE("A raider cannot exceed two cargo portions even across multiple completed thefts", "[AIWorld][LivingSupply][Raiders]")
{
    auto ledger = RaidFixture();
    REQUIRE(ledger.Steal(301, 1, 1, 1, true).Portions == 1);
    auto fill = ledger.Steal(301, 2, 1, 2, true);
    REQUIRE(fill.Success);
    REQUIRE(fill.Portions == 1);
    REQUIRE(ledger.GetCargo(301) == RaiderCargoCapacity);
    REQUIRE_FALSE(ledger.Steal(301, 3, 1, 1, true).Success);
    REQUIRE(ledger.GetDepot(1)->Stock == 4);
    REQUIRE(ledger.GetCounters().StolenTotal == 2);
    REQUIRE(ledger.Deliver(301, 3, 3, 2, true).Portions == 2);
    REQUIRE(ledger.Steal(301, 4, 1, 2, true).Portions == 2);
    REQUIRE(ledger.GetCargo(301) == 2);
    REQUIRE(ledger.GetDepot(1)->Stock == 2);
    REQUIRE(ledger.GetDepot(3)->Stock == 2);
    REQUIRE(ledger.GetCounters().StolenTotal == 4);
    Conserved(ledger);
}

TEST_CASE("Two raiders cannot both claim the same final depot portions", "[AIWorld][LivingSupply][Raiders]")
{
    auto ledger = RaidFixture(2);
    REQUIRE(ledger.RegisterRaider(302));
    REQUIRE(ledger.Steal(301, 1, 1, 2, true).Success);
    auto second = ledger.Steal(302, 1, 1, 2, true);
    REQUIRE_FALSE(second.Success);
    REQUIRE(std::string(second.Reason) == "NO_FOOD");
    REQUIRE(ledger.GetCargo(301) == 2);
    REQUIRE(ledger.GetCargo(302) == 0);
    REQUIRE(ledger.GetDepot(1)->Stock == 0);
    REQUIRE(ledger.GetCounters().StolenTotal == 2);
    Conserved(ledger);
}

TEST_CASE("A meal racing a theft can consume the last portion without a partial theft debit", "[AIWorld][LivingSupply][Raiders]")
{
    auto ledger = RaidFixture(2);
    REQUIRE(ledger.RegisterActor(302, 1, 0, false, true));
    REQUIRE(ledger.EatDepot(302, 1, 1, 1, true).Success);
    REQUIRE_FALSE(ledger.Steal(301, 1, 1, 2, true).Success);
    REQUIRE(ledger.GetDepot(1)->Stock == 1);
    REQUIRE(ledger.GetCargo(301) == 0);
    REQUIRE(ledger.GetCounters().StolenTotal == 0);
    REQUIRE(ledger.Steal(301, 1, 1, 1, true).Success);
    REQUIRE_FALSE(ledger.EatDepot(302, 2, 1, 1, true).Success);
    REQUIRE(ledger.GetCargo(301) == 1);
    REQUIRE(ledger.GetDepot(1)->Stock == 0);
    REQUIRE(ledger.GetCounters().ConsumedTotal == 1);
    REQUIRE(ledger.GetCounters().StolenTotal == 1);
    Conserved(ledger);
}

TEST_CASE("Carrying stolen food through death records one loss and cannot replay the previous life's theft", "[AIWorld][LivingSupply][Raiders]")
{
    auto ledger = RaidFixture();
    REQUIRE_FALSE(ledger.Steal(301, 1, 1, 2, false).Success);
    REQUIRE(ledger.DropCargo(301) == 0);
    REQUIRE(ledger.GetDepot(1)->Stock == 6);
    REQUIRE(ledger.GetCounters().LostTotal == 0);
    REQUIRE(ledger.Steal(301, 1, 1, 2, true).Success);
    REQUIRE(ledger.DropCargo(301) == 2);
    REQUIRE(ledger.DropCargo(301) == 0);
    REQUIRE(ledger.GetCounters().LostTotal == 2);
    REQUIRE(ledger.GetCounters().StolenTotal == 2);
    REQUIRE(ledger.GetCargo(301) == 0);
    REQUIRE_FALSE(ledger.Steal(301, 1, 1, 2, true).Success);
    REQUIRE_FALSE(ledger.Deliver(301, 2, 3, 2, true).Success);
    REQUIRE(ledger.Steal(301, 2, 1, 2, true).Success);
    REQUIRE(ledger.Deliver(301, 3, 3, 2, true).Success);
    REQUIRE(ledger.GetDepot(1)->Stock == 2);
    REQUIRE(ledger.GetDepot(3)->Stock == 2);
    REQUIRE(ledger.GetCounters().LostTotal == 2);
    REQUIRE(ledger.GetCounters().StolenTotal == 4);
    Conserved(ledger);
}

TEST_CASE("Raided harvested food remains conserved through the foreign depot camp and meals", "[AIWorld][LivingSupply][Raiders]")
{
    auto ledger = RaidFixture(0);
    REQUIRE(ledger.RegisterActor(302, 1, 0, false, true));
    Harvest(ledger, 101, {401, 1});
    REQUIRE(ledger.Deliver(101, 1, 1, 6, true).Portions == 6);
    REQUIRE(ledger.Steal(301, 1, 1, 2, true).Portions == 2);
    REQUIRE(ledger.EatDepot(302, 1, 1, 1, true).Success);
    REQUIRE(ledger.Deliver(301, 2, 3, 2, true).Portions == 2);
    REQUIRE(ledger.EatDepot(301, 3, 3, 1, true).Success);
    REQUIRE(ledger.GetCounters().InitialFood == 0);
    REQUIRE(ledger.GetCounters().HarvestedTotal == 6);
    REQUIRE(ledger.GetCounters().StolenTotal == 2);
    REQUIRE(ledger.GetCounters().DeliveredTotal == 8); // Six original + two transferred.
    REQUIRE(ledger.GetCounters().ConsumedTotal == 2);
    REQUIRE(ledger.DepotFood() == 4);
    REQUIRE(ledger.CargoFood() == 0);
    Conserved(ledger);
}

TEST_CASE("A full raider camp preserves stolen cargo until a real meal creates delivery capacity", "[AIWorld][LivingSupply][Raiders]")
{
    auto ledger = SupplyFixture(6);
    REQUIRE(ledger.ConfigureDepot(3, 30));
    REQUIRE(ledger.RegisterRaider(301));
    REQUIRE(ledger.Steal(301, 1, 1, 2, true).Success);
    auto full = ledger.Deliver(301, 2, 3, 2, true);
    REQUIRE_FALSE(full.Success);
    REQUIRE(std::string(full.Reason) == "DEPOT_FULL");
    REQUIRE(ledger.GetCargo(301) == 2);
    REQUIRE_FALSE(ledger.EatDepot(101, 1, 3, 1, true).Success);
    REQUIRE_FALSE(ledger.Deliver(101, 1, 3, 1, true).Success);
    REQUIRE(ledger.EatDepot(301, 2, 3, 1, true).Success);
    REQUIRE_FALSE(ledger.Deliver(301, 2, 3, 2, true).Success);
    auto partial = ledger.Deliver(301, 3, 3, 2, true);
    REQUIRE(partial.Success);
    REQUIRE(partial.Portions == 1);
    REQUIRE(ledger.Deliver(301, 3, 3, 2, true).Duplicate);
    REQUIRE(ledger.GetCargo(301) == 1);
    REQUIRE(ledger.GetDepot(3)->Stock == 30);
    REQUIRE(ledger.GetCounters().StolenTotal == 2);
    REQUIRE(ledger.GetCounters().DeliveredTotal == 1);
    REQUIRE(ledger.GetCounters().ConsumedTotal == 1);
    REQUIRE(ledger.DropCargo(301) == 1);
    REQUIRE(ledger.GetCounters().LostTotal == 1);
    Conserved(ledger);
}
