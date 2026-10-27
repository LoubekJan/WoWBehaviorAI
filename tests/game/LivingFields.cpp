/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/LivingFieldRuntime.h"
#include "Agent/LivingSupplyRuntime.h"
#include "Agent/LivingSupplyState.h"
#include "Needs/NeedsState.h"
#include <cmath>
#include <limits>
#include <set>
#include <string>

namespace
{
    using namespace LivingSupply;

    Ledger FieldFixture(uint32 ownStock = 0, uint32 crops = FieldCapacity)
    {
        Ledger ledger;
        REQUIRE(ledger.ConfigureDepot(1, 6));
        REQUIRE(ledger.ConfigureDepot(2, 5));
        REQUIRE(ledger.ConfigureDepot(3, 4));
        for (auto const& field : LivingFields::Fields)
        {
            REQUIRE(ledger.ConfigureDepot(field.Depot, field.Id == 4 ? ownStock : 0));
            REQUIRE(ledger.ConfigureField(field.Id, 100, crops));
        }
        for (auto const& worker : LivingFields::Workers)
            REQUIRE(ledger.RegisterFarmer(worker.Spawn, worker.Depot, worker.Field));
        REQUIRE(ledger.RegisterActor(101, 1));
        REQUIRE(ledger.RegisterActor(201, 0, 0, true));
        REQUIRE(ledger.RegisterActor(202, 1, 0, false, true));
        REQUIRE(ledger.RegisterRaider(301));
        return ledger;
    }

    void FieldConserved(Ledger const& ledger)
    {
        REQUIRE(ledger.Conserved());
        REQUIRE(ledger.PotentialConserved());
    }
}

TEST_CASE("Only the twelve reviewed village workers acquire field capability", "[AIWorld][LivingFields][Catalog]")
{
    std::set<uint64> accepted;
    std::set<uint32> depots;
    for (auto const& worker : LivingFields::Workers)
    {
        REQUIRE(accepted.insert(worker.Spawn).second);
        REQUIRE(LivingFields::Worker(worker.Spawn) == &worker);
        REQUIRE(LivingFields::Capability(worker.Spawn, 725, AgentType::Civilian, AgentControlMode::AIWorldControlled, true));
        REQUIRE_FALSE(LivingFields::Capability(worker.Spawn, 0, AgentType::Civilian, AgentControlMode::AIWorldControlled, true));
        REQUIRE_FALSE(LivingFields::Capability(worker.Spawn, 725, AgentType::Combatant, AgentControlMode::AIWorldControlled, true));
        REQUIRE_FALSE(LivingFields::Capability(worker.Spawn, 725, AgentType::Predator, AgentControlMode::AIWorldControlled, true));
        REQUIRE_FALSE(LivingFields::Capability(worker.Spawn, 725, AgentType::Civilian, AgentControlMode::ObserveOnly, true));
        REQUIRE_FALSE(LivingFields::Capability(worker.Spawn, 725, AgentType::Civilian, AgentControlMode::AIWorldControlled, false));
        uint32 expected = worker.Spawn <= 901019 ? 4 : worker.Spawn <= 901025 ? 5 : 6;
        REQUIRE(worker.Field == expected);
        REQUIRE(worker.Depot == expected);
        depots.insert(worker.Depot);
    }
    REQUIRE(accepted.size() == 12);
    REQUIRE((depots == std::set<uint32>{4, 5, 6}));
    for (uint64 native : {0ull, 901015ull, 901020ull, 901021ull, 901026ull, 901027ull, 901032ull,
        901033ull, 901034ull, 901035ull, 901036ull, 901037ull, 901063ull, 901100ull})
    {
        REQUIRE(LivingFields::Worker(native) == nullptr);
        REQUIRE_FALSE(LivingFields::Capability(native, 725, AgentType::Civilian, AgentControlMode::AIWorldControlled, true));
    }
    REQUIRE(LivingFields::Field(1) == nullptr);
    REQUIRE(LivingFields::Field(7) == nullptr);
}

TEST_CASE("Each four-worker field rotates over twelve distinct crop-interior points", "[AIWorld][LivingFields][Catalog]")
{
    struct Bounds { uint32 Id; float MinX, MaxX, MinY, MaxY; };
    Bounds const bounds[]{{4, 347.3958f, 405.2084f, 544.7916f, 589.5834f},
        {5, 14.5833f, 59.8959f, 771.8749f, 819.7917f}, {6, 122.3958f, 185.9375f, 323.4374f, 371.8750f}};
    for (auto const& bound : bounds)
    {
        auto field = LivingFields::Field(bound.Id);
        REQUIRE(field);
        REQUIRE(field->Depot == bound.Id);
        REQUIRE(field->DepotPoint.MapId == 725);
        std::set<ActionPosition const*> visited;
        for (uint64 cycle = 0; cycle < 3; ++cycle)
            for (auto const& worker : LivingFields::Workers)
                if (worker.Field == bound.Id)
                {
                    auto point = LivingFields::WorkPoint(worker, cycle);
                    REQUIRE(point);
                    REQUIRE(visited.insert(point).second);
                    REQUIRE(point->MapId == 725);
                    REQUIRE(std::isfinite(point->X));
                    REQUIRE(std::isfinite(point->Y));
                    REQUIRE(point->Z == 0.0f);
                    REQUIRE(point->X > bound.MinX);
                    REQUIRE(point->X < bound.MaxX);
                    REQUIRE(point->Y > bound.MinY);
                    REQUIRE(point->Y < bound.MaxY);
                    REQUIRE(LivingFields::WorkPoint(worker, cycle + 3) == point);
                }
        REQUIRE(visited.size() == 12);
    }
    REQUIRE(LivingFields::Field(4)->DepotPoint.X == Approx(378.0363521f));
    REQUIRE(LivingFields::Field(5)->DepotPoint.Y == Approx(859.9705627f));
    REQUIRE(LivingFields::Field(6)->DepotPoint.X == Approx(151.1844024f));
    REQUIRE(LivingFields::WorkPoint({901016, 1, 1, 0}, 0) == nullptr);
    REQUIRE(LivingFields::WorkPoint({901016, 4, 5, 0}, 0) == nullptr);
    REQUIRE(LivingFields::WorkPoint({901016, 4, 4, 4}, 0) == nullptr);
}

TEST_CASE("Crop configuration is bounded idempotent and creates only potential", "[AIWorld][LivingFields][Ledger]")
{
    Ledger ledger;
    REQUIRE_FALSE(ledger.ConfigureField(4, 100));
    for (uint32 id = 1; id <= 6; ++id)
        REQUIRE(ledger.ConfigureDepot(id));
    REQUIRE_FALSE(ledger.ConfigureDepot(7));
    for (uint32 invalid : {0u, 1u, 2u, 3u, 7u})
        REQUIRE_FALSE(ledger.ConfigureField(invalid, 100));
    REQUIRE_FALSE(ledger.ConfigureField(4, 100, 0, 0));
    REQUIRE_FALSE(ledger.ConfigureField(4, 100, 25, 25));
    REQUIRE_FALSE(ledger.ConfigureField(4, 100, 25));
    REQUIRE_FALSE(ledger.ConfigureField(4, 100, 0, 24, 29999));
    REQUIRE(ledger.ConfigureField(4, 100));
    REQUIRE(ledger.ConfigureField(5, 100));
    REQUIRE(ledger.ConfigureField(6, 100));
    REQUIRE(ledger.ConfigureField(4, 99999));
    REQUIRE_FALSE(ledger.ConfigureField(4, 100, 23));
    REQUIRE_FALSE(ledger.ConfigureField(4, 100, 24, 24, 60000));
    REQUIRE(ledger.GetCounters().InitialFood == 0);
    REQUIRE(ledger.GetCounters().HarvestedTotal == 0);
    REQUIRE(ledger.GetCounters().PotentialCreated == 72);
    REQUIRE(ledger.RemainingPotential() == 72);
    REQUIRE(ledger.DepotFood() == 0);
    REQUIRE(ledger.CargoFood() == 0);
    REQUIRE(ledger.GetField(4)->LastGrowthAtMs == 100);
    REQUIRE(ledger.GetField(4)->DepotId == 4);
    REQUIRE(ledger.GetField(1) == nullptr);
    FieldConserved(ledger);
}

TEST_CASE("Farmers cannot upgrade capabilities or access foreign field meat or warehouse sources", "[AIWorld][LivingFields][Ledger]")
{
    auto ledger = FieldFixture(2);
    REQUIRE_FALSE(ledger.RegisterFarmer(0, 4, 4));
    REQUIRE_FALSE(ledger.RegisterFarmer(401, 1, 4));
    REQUIRE_FALSE(ledger.RegisterFarmer(401, 4, 5));
    REQUIRE_FALSE(ledger.RegisterFarmer(401, 4, 4, 0));
    REQUIRE_FALSE(ledger.RegisterFarmer(401, 4, 4, 3));
    REQUIRE(ledger.RegisterFarmer(901016, 4, 4));
    REQUIRE_FALSE(ledger.RegisterFarmer(901016, 5, 5));
    REQUIRE_FALSE(ledger.RegisterActor(901016, 4, 2));
    REQUIRE_FALSE(ledger.RegisterActor(901016, 4, 0, false, true));
    REQUIRE_FALSE(ledger.RegisterRaider(901016));
    REQUIRE(ledger.RegisterActor(401, 4, 2));
    for (uint64 actor : {101ull, 201ull, 202ull, 301ull, 401ull})
    {
        REQUIRE_FALSE(ledger.RegisterFarmer(actor, 4, 4));
        REQUIRE_FALSE(ledger.HarvestField(actor, 1, 4, 1, 100, true).Success);
    }
    REQUIRE(ledger.RegisterCarcass({701, 1}, 100, 10000));
    for (auto const& worker : LivingFields::Workers)
    {
        REQUIRE_FALSE(ledger.Reserve(worker.Spawn, {701, 1}, ClaimKind::Harvest, 1, 100).has_value());
        REQUIRE_FALSE(ledger.Reserve(worker.Spawn, {701, 1}, ClaimKind::PredatorMeal, 1, 100).has_value());
        REQUIRE_FALSE(ledger.Steal(worker.Spawn, 1, 1, 1, true).Success);
        for (uint32 id = 1; id <= 6; ++id)
            if (id != worker.Depot)
            {
                REQUIRE_FALSE(ledger.HarvestField(worker.Spawn, 1, id, 1, 100, true).Success);
                REQUIRE_FALSE(ledger.Deliver(worker.Spawn, 1, id, 1, true).Success);
                REQUIRE_FALSE(ledger.EatDepot(worker.Spawn, 1, id, 1, true).Success);
            }
    }
    REQUIRE_FALSE(ledger.HarvestField(999, 1, 4, 1, 100, true).Success);
    REQUIRE_FALSE(ledger.HarvestField(901016, 0, 4, 1, 100, true).Success);
    REQUIRE_FALSE(ledger.HarvestField(901016, 1, 4, 0, 100, true).Success);
    REQUIRE_FALSE(ledger.HarvestField(901016, 1, 4, 3, 100, true).Success);
    REQUIRE_FALSE(ledger.HarvestField(901016, 1, 4, 1, 100, false).Success);
    REQUIRE(ledger.GetCarcass({701, 1})->Remaining == 6);
    REQUIRE(ledger.GetField(4)->Remaining == 24);
    REQUIRE(ledger.GetDepot(1)->Stock == 6);
    REQUIRE(ledger.GetDepot(4)->Stock == 2);
    REQUIRE(ledger.GetCounters().HarvestedTotal == 0);
    FieldConserved(ledger);
}

TEST_CASE("Harvest delivery and meal share one receipt watermark across death", "[AIWorld][LivingFields][Ledger]")
{
    auto ledger = FieldFixture();
    auto harvest = ledger.HarvestField(901016, 1, 4, 2, 100, true);
    REQUIRE(harvest.Success);
    REQUIRE(harvest.Portions == 2);
    REQUIRE_FALSE(harvest.Duplicate);
    REQUIRE(ledger.HarvestField(901016, 1, 4, 2, 100, true).Duplicate);
    REQUIRE_FALSE(ledger.HarvestField(901016, 1, 4, 1, 100, true).Success);
    REQUIRE_FALSE(ledger.HarvestField(901016, 1, 5, 2, 100, true).Success);
    REQUIRE_FALSE(ledger.Deliver(901016, 1, 4, 2, true).Success);
    REQUIRE_FALSE(ledger.EatDepot(901016, 1, 4, 1, true).Success);
    REQUIRE(ledger.GetCargo(901016) == 2);
    REQUIRE(ledger.GetField(4)->Remaining == 22);
    REQUIRE(ledger.ConfigureField(4, 99999));
    REQUIRE(ledger.GetField(4)->Remaining == 22);
    REQUIRE(ledger.GetCounters().PotentialCreated == 72);
    REQUIRE(ledger.Deliver(901016, 2, 4, 2, true).Portions == 2);
    REQUIRE(ledger.Deliver(901016, 2, 4, 2, true).Duplicate);
    REQUIRE_FALSE(ledger.HarvestField(901016, 1, 4, 2, 100, true).Success);
    REQUIRE_FALSE(ledger.HarvestField(901016, 2, 4, 2, 100, true).Success);
    REQUIRE(ledger.EatDepot(901016, 3, 4, 1, true).Success);
    REQUIRE(ledger.EatDepot(901016, 3, 4, 1, true).Duplicate);
    REQUIRE_FALSE(ledger.HarvestField(901016, 3, 4, 1, 100, true).Success);
    REQUIRE(ledger.HarvestField(901016, 4, 4, 1, 100, true).Success);
    REQUIRE(ledger.DropCargo(901016) == 1);
    REQUIRE(ledger.DropCargo(901016) == 0);
    REQUIRE_FALSE(ledger.HarvestField(901016, 4, 4, 1, 100, true).Success);
    REQUIRE_FALSE(ledger.Deliver(901016, 5, 4, 1, true).Success);
    REQUIRE(ledger.HarvestField(901016, 5, 4, 1, 100, true).Success);
    REQUIRE(ledger.GetField(4)->Remaining == 20);
    REQUIRE(ledger.GetCounters().HarvestedTotal == 4);
    REQUIRE(ledger.GetCounters().DeliveredTotal == 2);
    REQUIRE(ledger.GetCounters().ConsumedTotal == 1);
    REQUIRE(ledger.GetCounters().LostTotal == 1);
    REQUIRE(ledger.GetCargo(901016) == 1);
    FieldConserved(ledger);
}

TEST_CASE("Full cargo and full warehouse preserve crops or cargo until capacity exists", "[AIWorld][LivingFields][Ledger]")
{
    auto ledger = FieldFixture(30);
    REQUIRE(ledger.HarvestField(901016, 1, 4, 2, 100, true).Portions == 2);
    auto fullCargo = ledger.HarvestField(901016, 2, 4, 1, 100, true);
    REQUIRE_FALSE(fullCargo.Success);
    REQUIRE(std::string(fullCargo.Reason) == "CARGO_FULL");
    REQUIRE(ledger.GetField(4)->Remaining == 22);
    auto fullDepot = ledger.Deliver(901016, 2, 4, 2, true);
    REQUIRE_FALSE(fullDepot.Success);
    REQUIRE(std::string(fullDepot.Reason) == "DEPOT_FULL");
    REQUIRE(ledger.GetCargo(901016) == 2);
    REQUIRE(ledger.EatDepot(901016, 2, 4, 1, true).Success);
    REQUIRE_FALSE(ledger.Deliver(901016, 2, 4, 2, true).Success);
    REQUIRE(ledger.Deliver(901016, 3, 4, 2, true).Portions == 1);
    REQUIRE(ledger.Deliver(901016, 3, 4, 2, true).Duplicate);
    REQUIRE(ledger.GetCargo(901016) == 1);
    REQUIRE(ledger.GetDepot(4)->Stock == 30);
    REQUIRE(ledger.HarvestField(901016, 4, 4, 2, 100, true).Portions == 1);
    REQUIRE(ledger.GetCargo(901016) == FarmerCargoCapacity);
    REQUIRE(ledger.GetField(4)->Remaining == 21);
    REQUIRE(ledger.DropCargo(901016) == 2);
    REQUIRE(ledger.GetCounters().LostTotal == 2);
    FieldConserved(ledger);
}

TEST_CASE("Two completed workers cannot both harvest the final crop portion", "[AIWorld][LivingFields][Ledger]")
{
    auto ledger = FieldFixture(0, 1);
    REQUIRE(ledger.HarvestField(901016, 1, 4, 2, 100, true).Portions == 1);
    auto empty = ledger.HarvestField(901017, 1, 4, 1, 100, true);
    REQUIRE_FALSE(empty.Success);
    REQUIRE(std::string(empty.Reason) == "FIELD_EMPTY");
    REQUIRE(ledger.GetField(4)->Remaining == 0);
    REQUIRE(ledger.GetField(5)->Remaining == 1);
    REQUIRE(ledger.GetField(6)->Remaining == 1);
    REQUIRE(ledger.GetCargo(901016) == 1);
    REQUIRE(ledger.GetCargo(901017) == 0);
    REQUIRE(ledger.GetCounters().HarvestedTotal == 1);
    FieldConserved(ledger);
}

TEST_CASE("Field growth is bounded uses elapsed intervals and discards time spent full", "[AIWorld][LivingFields][Regrowth]")
{
    Ledger ledger;
    REQUIRE(ledger.ConfigureDepot(4));
    REQUIRE(ledger.ConfigureField(4, 1000, 0, 3));
    REQUIRE(ledger.RegisterFarmer(901016, 4, 4));
    ledger.Expire(30999);
    REQUIRE(ledger.GetField(4)->Remaining == 0);
    ledger.Expire(31000);
    REQUIRE(ledger.GetField(4)->Remaining == 1);
    ledger.Expire(76000);
    REQUIRE(ledger.GetField(4)->Remaining == 2);
    REQUIRE(ledger.GetField(4)->LastGrowthAtMs == 61000);
    ledger.Expire(91000);
    REQUIRE(ledger.GetField(4)->Remaining == 3);
    ledger.Expire(1000000);
    REQUIRE(ledger.GetField(4)->Remaining == 3);
    REQUIRE(ledger.GetCounters().PotentialCreated == 3);
    REQUIRE(ledger.GetCounters().HarvestedTotal == 0);
    REQUIRE(ledger.HarvestField(901016, 1, 4, 1, 1000000, true).Success);
    REQUIRE(ledger.GetField(4)->Remaining == 2);
    ledger.Expire(1000001);
    ledger.Expire(1029999);
    REQUIRE(ledger.GetField(4)->Remaining == 2);
    ledger.Expire(1030000);
    REQUIRE(ledger.GetField(4)->Remaining == 3);
    REQUIRE(ledger.GetField(4)->RegrownTotal == 4);
    REQUIRE(ledger.GetCounters().PotentialCreated == 4);
    REQUIRE(ledger.GetCounters().HarvestedTotal == 1);
    FieldConserved(ledger);
}

TEST_CASE("Backwards time and extreme elapsed time cannot bank free crop growth", "[AIWorld][LivingFields][Regrowth]")
{
    auto ledger = FieldFixture();
    ledger.Expire(300100);
    REQUIRE(ledger.GetField(4)->LastGrowthAtMs == 300100);
    REQUIRE(ledger.HarvestField(901016, 1, 4, 2, 300100, true).Success);
    REQUIRE(ledger.DropCargo(901016) == 2);
    ledger.Expire(100);
    REQUIRE(ledger.HarvestField(901016, 2, 4, 1, 100, true).Success);
    REQUIRE(ledger.GetField(4)->LastGrowthAtMs == 300100);
    REQUIRE(ledger.GetField(4)->Remaining == 21);
    ledger.Expire(330099);
    REQUIRE(ledger.GetField(4)->Remaining == 21);
    ledger.Expire(330100);
    REQUIRE(ledger.GetField(4)->Remaining == 22);
    REQUIRE(ledger.GetCounters().PotentialCreated == 73);
    auto maximum = std::numeric_limits<uint64>::max();
    ledger.Expire(maximum);
    REQUIRE(ledger.GetField(4)->Remaining == 24);
    REQUIRE(ledger.GetField(4)->LastGrowthAtMs == maximum);
    REQUIRE(ledger.GetCounters().PotentialCreated == 75);
    REQUIRE(ledger.HarvestField(901016, 3, 4, 1, maximum, true).Success);
    ledger.Expire(0);
    ledger.Expire(maximum);
    REQUIRE(ledger.GetField(4)->Remaining == 23);
    REQUIRE(ledger.GetCounters().PotentialCreated == 75);
    FieldConserved(ledger);
}

TEST_CASE("Twelve farmers preserve shared conservation through finite crops meat theft and meals", "[AIWorld][LivingFields][Conservation]")
{
    auto ledger = FieldFixture();
    for (uint64 cycle = 0; cycle < 3; ++cycle)
        for (auto const& worker : LivingFields::Workers)
        {
            uint64 serial = 3 * cycle + 1;
            REQUIRE(ledger.HarvestField(worker.Spawn, serial, worker.Field, 2, 100, true).Portions == 2);
            REQUIRE(ledger.Deliver(worker.Spawn, serial + 1, worker.Depot, 2, true).Portions == 2);
            REQUIRE(ledger.EatDepot(worker.Spawn, serial + 2, worker.Depot, 1, true).Success);
            FieldConserved(ledger);
        }
    for (auto const& field : LivingFields::Fields)
    {
        REQUIRE(ledger.GetField(field.Id)->Remaining == 0);
        REQUIRE(ledger.GetField(field.Id)->HarvestedTotal == 24);
        REQUIRE(ledger.GetDepot(field.Depot)->Stock == 12);
    }
    for (auto const& worker : LivingFields::Workers)
        REQUIRE_FALSE(ledger.HarvestField(worker.Spawn, 10, worker.Field, 1, 100, true).Success);
    REQUIRE(ledger.GetCounters().HarvestedTotal == 72);
    REQUIRE(ledger.GetCounters().ConsumedTotal == 36);
    REQUIRE(ledger.RegisterCarcass({701, 1}, 100, 10000));
    auto claim = ledger.Reserve(101, {701, 1}, ClaimKind::Harvest, 6, 100);
    REQUIRE(claim.has_value());
    REQUIRE(ledger.Commit(*claim, 100).Portions == 6);
    REQUIRE(ledger.Deliver(101, 1, 1, 6, true).Portions == 6);
    REQUIRE(ledger.Steal(301, 1, 1, 2, true).Portions == 2);
    REQUIRE(ledger.Deliver(301, 2, 3, 2, true).Portions == 2);
    REQUIRE(ledger.EatDepot(301, 3, 3, 1, true).Success);
    REQUIRE(ledger.GetCounters().HarvestedTotal == 78);
    REQUIRE(ledger.GetCounters().StolenTotal == 2);
    REQUIRE(ledger.GetDepot(1)->Stock == 10);
    REQUIRE(ledger.GetDepot(2)->Stock == 5);
    REQUIRE(ledger.GetDepot(3)->Stock == 5);
    FieldConserved(ledger);
    ledger.Expire(30100);
    REQUIRE(ledger.RemainingPotential() == 3);
    REQUIRE(ledger.GetCounters().PotentialCreated == 81);
    REQUIRE(ledger.GetCounters().HarvestedTotal == 78);
    FieldConserved(ledger);
}

TEST_CASE("A finite field work cycle carries two portions to its warehouse then returns home", "[AIWorld][LivingFields][RuntimePolicy]")
{
    auto ledger = FieldFixture();
    NeedsState needs;
    needs.Hunger = 0.3f;
    auto depot = ledger.GetDepot(4);
    REQUIRE(ledger.HarvestField(901016, 1, 4, 1, 100, true).Success);
    REQUIRE(FieldStageAfterWork(ledger.GetCargo(901016), needs.Hunger >= 0.65f,
        depot->Stock, depot->Capacity, true) == FieldStage::Field);
    REQUIRE(ledger.HarvestField(901016, 2, 4, 1, 100, true).Success);
    REQUIRE(FieldStageAfterWork(ledger.GetCargo(901016), needs.Hunger >= 0.65f,
        depot->Stock, depot->Capacity, true) == FieldStage::Warehouse);
    REQUIRE(ledger.Deliver(901016, 3, 4, 2, true).Portions == 2);
    REQUIRE(FieldStageAfterWarehouse(ledger.GetCargo(901016), needs.Hunger >= 0.65f,
        depot->Stock, depot->Capacity) == FieldStage::Home);
    REQUIRE(ledger.GetCargo(901016) == 0);
    REQUIRE(depot->Stock == 2);
    REQUIRE(needs.Hunger == Approx(0.3f));
    REQUIRE(ledger.GetCounters().ConsumedTotal == 0);
    FieldConserved(ledger);
}

TEST_CASE("Maintenance and blocked storage finish a field visit while preserving held cargo", "[AIWorld][LivingFields][RuntimePolicy]")
{
    auto ledger = FieldFixture(30);
    REQUIRE(ledger.HarvestField(901016, 1, 4, 2, 100, true).Success);
    auto depot = ledger.GetDepot(4);
    auto potentialBefore = ledger.RemainingPotential();
    REQUIRE(FieldStageAfterWork(2, false, depot->Stock, depot->Capacity, false) == FieldStage::Home);
    REQUIRE(FieldStageAfterWarehouse(2, false, depot->Stock, depot->Capacity) == FieldStage::Home);
    REQUIRE(FieldStageAfterWork(1, false, 29, 30, false) == FieldStage::Home);
    REQUIRE(FieldStageAfterWork(0, false, 0, 30, false) == FieldStage::Home);
    REQUIRE(FieldStageAfterWork(0, true, 0, 30, false) == FieldStage::Home);
    REQUIRE(ledger.GetCargo(901016) == 2);
    REQUIRE(ledger.RemainingPotential() == potentialBefore);
    REQUIRE(ledger.GetCounters().LostTotal == 0);
    REQUIRE(ledger.GetCounters().ConsumedTotal == 0);
    // A full warehouse can still provide an actual stock meal to a hungry
    // worker, and the newly available slot then permits a partial delivery.
    REQUIRE(FieldStageAfterWork(2, true, depot->Stock, depot->Capacity, false) == FieldStage::Warehouse);
    REQUIRE(ledger.EatDepot(901016, 2, 4, 1, true).Success);
    REQUIRE(FieldStageAfterWarehouse(2, false, depot->Stock, depot->Capacity) == FieldStage::Warehouse);
    REQUIRE(ledger.Deliver(901016, 3, 4, 2, true).Portions == 1);
    REQUIRE(FieldStageAfterWarehouse(1, false, depot->Stock, depot->Capacity) == FieldStage::Home);
    REQUIRE(ledger.GetCargo(901016) == 1);
    REQUIRE(ledger.GetCounters().LostTotal == 0);
    FieldConserved(ledger);
}

TEST_CASE("Hungry workers deliver cargo before an owned stock meal and never eat cargo directly", "[AIWorld][LivingFields][RuntimePolicy]")
{
    auto ledger = FieldFixture();
    NeedsState needs;
    needs.Hunger = 0.8f;
    auto depot = ledger.GetDepot(4);
    REQUIRE(ledger.HarvestField(901016, 1, 4, 1, 100, true).Success);
    REQUIRE(FieldStageAfterWork(1, needs.Hunger >= 0.65f, depot->Stock, depot->Capacity, true) == FieldStage::Warehouse);
    REQUIRE_FALSE(ledger.EatDepot(901016, 2, 4, 1, true).Success);
    REQUIRE(ledger.GetCargo(901016) == 1);
    REQUIRE(depot->Stock == 0);
    REQUIRE(ledger.GetCounters().ConsumedTotal == 0);
    REQUIRE(FieldStageAfterWarehouse(1, needs.Hunger >= 0.65f, depot->Stock, depot->Capacity) == FieldStage::Warehouse);
    REQUIRE(needs.Hunger == Approx(0.8f));
    REQUIRE(ledger.Deliver(901016, 2, 4, 1, true).Success);
    REQUIRE(ledger.GetCargo(901016) == 0);
    REQUIRE(needs.Hunger == Approx(0.8f));
    REQUIRE(FieldStageAfterWarehouse(0, needs.Hunger >= 0.65f, depot->Stock, depot->Capacity) == FieldStage::Warehouse);
    REQUIRE_FALSE(ledger.EatDepot(901016, 3, 4, 1, false).Success);
    REQUIRE(depot->Stock == 1);
    REQUIRE(needs.Hunger == Approx(0.8f));
    REQUIRE(ledger.EatDepot(901016, 3, 4, 1, true).Success);
    REQUIRE(ledger.GetCounters().ConsumedTotal == 1);
    // Satisfaction is applied by the live runtime only after that successful
    // receipt; planning itself accepts no mutable needs state.
    REQUIRE(needs.Hunger == Approx(0.8f));
    REQUIRE(FieldStageAfterWarehouse(0, false, depot->Stock, depot->Capacity) == FieldStage::Home);
    FieldConserved(ledger);
}

TEST_CASE("Hungry warehouse planning respects own stock and retries the field when it is empty", "[AIWorld][LivingFields][RuntimePolicy]")
{
    auto ledger = FieldFixture();
    NeedsState needs;
    needs.Hunger = 0.8f;
    auto depot = ledger.GetDepot(4);
    REQUIRE(ledger.GetDepot(1)->Stock == 6);
    REQUIRE(FieldStageAfterWarehouse(0, true, depot->Stock, depot->Capacity) == FieldStage::Field);
    REQUIRE(ledger.GetCargo(901016) == 0);
    REQUIRE_FALSE(ledger.EatDepot(901016, 1, 1, 1, true).Success);
    REQUIRE(needs.Hunger == Approx(0.8f));
    REQUIRE(ledger.GetDepot(1)->Stock == 6);
    REQUIRE(ledger.GetCounters().ConsumedTotal == 0);
    REQUIRE(FieldStageAfterWork(0, true, 1, 30, false) == FieldStage::Warehouse);
    REQUIRE(FieldStageAfterWarehouse(0, true, 1, 30) == FieldStage::Warehouse);
    REQUIRE(FieldStageAfterWarehouse(0, false, 1, 30) == FieldStage::Home);
    REQUIRE(needs.Hunger == Approx(0.8f));
    FieldConserved(ledger);
}
