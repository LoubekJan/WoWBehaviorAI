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

TEST_CASE("Farmer warehouse arrival rejects the observed left-village collision-edge standoff", "[AIWorld][LivingFields][Arrival]")
{
    auto worker = LivingFields::Worker(901022);
    REQUIRE(worker);
    auto field = LivingFields::Field(worker->Field);
    REQUIRE(field);
    ActionPosition const home = field->DepotPoint;
    ActionPosition const stalled{725, 44.43344f, 857.5293f, 0.0f};
    float radius = LivingFields::ArrivalRadius(*worker, home, home);
    REQUIRE(radius == Approx(1.9f));
    REQUIRE(LivingReturnPolicy::Distance(stalled, home) < 3.0f);
    REQUIRE(LivingReturnPolicy::Distance(stalled, home) > radius);
    // The tight request removes the old two-yard ring targets responsible
    // for accepting this location as the completed warehouse approach.
    auto targets = LivingReturnPolicy::HomeTargets(home, radius);
    REQUIRE(targets.size() == 1);
    REQUIRE(LivingReturnPolicy::SamePosition(targets.front(), home));
    REQUIRE_FALSE(LivingReturnPolicy::HomeEndpointMatches(home, radius, stalled, stalled));
    REQUIRE(LivingReturnPolicy::UsefulStep(stalled, home));
    auto shortApproach = home;
    shortApproach.X += 1.1f;
    REQUIRE(LivingReturnPolicy::Distance(shortApproach, home) < radius);
    // Every point outside this anchor radius is beyond the existing useful
    // movement threshold; tightening does not introduce a dead annulus.
    auto outside = home;
    outside.X += radius + 0.01f;
    REQUIRE(LivingReturnPolicy::UsefulStep(outside, home));
}

TEST_CASE("Warehouse precision is limited to the registered worker's exact own warehouse", "[AIWorld][LivingFields][Arrival]")
{
    auto worker = LivingFields::Worker(901023);
    REQUIRE(worker);
    auto field = LivingFields::Field(worker->Field);
    REQUIRE(field);
    ActionPosition const home{725, 43.0f, 865.0f, 0.15f};
    REQUIRE(LivingFields::ArrivalRadius(*worker, home, home) == Approx(3.0f));
    REQUIRE(LivingFields::ArrivalRadius(*worker, field->DepotPoint, home) == Approx(1.9f));
    REQUIRE(LivingFields::ArrivalRadius(*worker, field->DepotPoint, field->DepotPoint) == Approx(1.9f));
    for (auto const& point : field->WorkPoints)
        REQUIRE(LivingFields::ArrivalRadius(*worker, point, home) == Approx(3.0f));
    for (auto const& foreign : LivingFields::Fields)
        if (foreign.Id != field->Id)
            REQUIRE(LivingFields::ArrivalRadius(*worker, foreign.DepotPoint, home) == Approx(3.0f));
    auto moved = field->DepotPoint;
    moved.X += 0.01f;
    REQUIRE(LivingFields::ArrivalRadius(*worker, moved, home) == Approx(3.0f));
    moved = field->DepotPoint;
    moved.MapId = 0;
    REQUIRE(LivingFields::ArrivalRadius(*worker, moved, home) == Approx(3.0f));
    moved = field->DepotPoint;
    moved.Z += 1.0f;
    REQUIRE(LivingFields::ArrivalRadius(*worker, moved, home) == Approx(3.0f));
    for (LivingFields::WorkerSpec forged : {LivingFields::WorkerSpec{901063, 5, 5, 1},
        LivingFields::WorkerSpec{901023, 6, 6, 1}, LivingFields::WorkerSpec{901023, 5, 5, 0}})
        REQUIRE(LivingFields::ArrivalRadius(forged, field->DepotPoint, home) == Approx(3.0f));
}

TEST_CASE("Farmer warehouse approach accepts supported terrain without ignoring three-dimensional distance", "[AIWorld][LivingFields][Arrival]")
{
    auto worker = LivingFields::Worker(901022);
    REQUIRE(worker);
    ActionPosition const home = LivingFields::Field(worker->Field)->DepotPoint;
    float radius = LivingFields::ArrivalRadius(*worker, home, home);
    auto ground = LivingReturnPolicy::GroundHomeTarget(home, home,
        [](ActionPosition const&) -> std::optional<float> { return 0.0f; },
        [](ActionPosition const&, ActionPosition const&) { return true; });
    REQUIRE(ground.has_value());
    REQUIRE(ground->Z == 0.0f);
    REQUIRE(LivingReturnPolicy::Distance(*ground, home) < radius);
    REQUIRE(LivingReturnPolicy::HomeEndpointMatches(home, radius, *ground, ground));
    auto elevated = *ground;
    elevated.Z = home.Z + 2.0f;
    REQUIRE(LivingReturnPolicy::Distance(elevated, home) > radius);
    REQUIRE_FALSE(LivingReturnPolicy::HomeEndpointMatches(home, radius, elevated, ground));
    // Search ownership includes the requested precision. A suspended ordinary
    // three-yard search cannot be resumed as proof of the tighter arrival.
    LivingReturnPolicy::HomeCorridorSearch search;
    ActionPosition const from{725, 32.25859f, 857.0308f, 0.0f};
    search.Begin(from, home, 3.0f, 384.0f, nullptr, 8.0f, true);
    REQUIRE_FALSE(search.Matches(from, home, radius, 384.0f, nullptr, 8.0f, true));
    search.Begin(from, home, radius, 384.0f, nullptr, 8.0f, true);
    REQUIRE(search.Matches(from, home, radius, 384.0f, nullptr, 8.0f, true));
}

TEST_CASE("Warehouse arrival covers the coarse detour grid cell including authored ground offset", "[AIWorld][LivingFields][Arrival]")
{
    auto worker = LivingFields::Worker(901022);
    REQUIRE(worker);
    ActionPosition const warehouse = LivingFields::Field(worker->Field)->DepotPoint;
    float radius = LivingFields::ArrivalRadius(*worker, warehouse, warehouse);
    float halfCell = LivingSurfaceDetour::GridStep / 2.0f;
    // A goal at a flat grid-cell center is farthest from every surrounding
    // node. Include the real 0.15-yard authored-home versus terrain offset.
    float worstDistance = std::hypot(halfCell, halfCell, warehouse.Z);
    REQUIRE(worstDistance == Approx(1.7741195f));
    REQUIRE(worstDistance > 1.25f);
    REQUIRE(worstDistance < radius);
    REQUIRE(radius == Approx(1.9f));
    auto nearest = warehouse;
    nearest.X += halfCell;
    nearest.Y += halfCell;
    nearest.Z = 0.0f;
    REQUIRE(LivingReturnPolicy::Distance(nearest, warehouse) < radius);
    REQUIRE(LivingReturnPolicy::HomeEndpointMatches(warehouse, radius, nearest, nearest));
}

TEST_CASE("Ordinary farmer home arrival covers the observed nearest nodes without changing warehouse precision", "[AIWorld][LivingFields][Arrival]")
{
    struct Fixture { uint64 Spawn; ActionPosition From; ActionPosition Home; };
    Fixture const fixtures[]{
        {901019, {725, 378.4412f, 614.4083f, 0.0f}, {725, 381.3137f, 595.6863f, 0.15f}},
        {901025, {725, 36.27481f, 850.8182f, 0.0f}, {725, 36.88912f, 829.6965f, 0.15f}}
    };
    for (auto const& fixture : fixtures)
    {
        auto worker = LivingFields::Worker(fixture.Spawn);
        REQUIRE(worker);
        auto field = LivingFields::Field(worker->Field);
        REQUIRE(field);
        float spacing = LivingSurfaceDetour::GridStep;
        ActionPosition nearest{725,
            fixture.From.X + spacing * std::round((fixture.Home.X - fixture.From.X) / spacing),
            fixture.From.Y + spacing * std::round((fixture.Home.Y - fixture.From.Y) / spacing), 0.0f};
        float distance = LivingReturnPolicy::Distance(nearest, fixture.Home);
        REQUIRE(distance > 1.25f);
        REQUIRE(distance < 1.30f);
        float radius = LivingFields::ArrivalRadius(*worker, fixture.Home, fixture.Home);
        REQUIRE(radius == Approx(3.0f));
        REQUIRE(distance < radius);
        REQUIRE(LivingReturnPolicy::HomeEndpointMatches(fixture.Home, radius, nearest, nearest));
        REQUIRE(LivingFields::ArrivalRadius(*worker, field->DepotPoint, fixture.Home) == Approx(1.9f));
    }
}

TEST_CASE("Canonical warehouse workers retain warehouse precision for their exact normalized immutable home", "[AIWorld][LivingFields][Arrival]")
{
    for (uint64 spawn : {901016ull, 901022ull, 901028ull})
    {
        auto worker = LivingFields::Worker(spawn);
        REQUIRE(worker);
        REQUIRE(worker->Ordinal == 0);
        auto field = LivingFields::Field(worker->Field);
        REQUIRE(field);
        auto actualHome = field->DepotPoint;
        actualHome.X = std::nextafter(actualHome.X, std::numeric_limits<float>::infinity());
        actualHome.Y = std::nextafter(actualHome.Y, -std::numeric_limits<float>::infinity());
        actualHome.Z = 0.0f;
        REQUIRE(LivingFields::ArrivalRadius(*worker, actualHome, actualHome) == Approx(1.9f));
        REQUIRE(LivingFields::ArrivalRadius(*worker, field->DepotPoint, actualHome) == Approx(1.9f));
        REQUIRE(LivingReturnPolicy::Distance(actualHome, field->DepotPoint) < 0.5f);
        // A nearby requested point cannot borrow immutable-home identity.
        auto nearHome = actualHome;
        nearHome.X += 0.001f;
        REQUIRE(LivingFields::ArrivalRadius(*worker, nearHome, actualHome) == Approx(3.0f));
        // Ground projection of an arbitrary warehouse request is not an exact
        // immutable-home request when the real home still has authored Z.
        REQUIRE(LivingFields::ArrivalRadius(*worker, actualHome, field->DepotPoint) == Approx(3.0f));
    }
}

TEST_CASE("Normalized-home precision cannot be borrowed by foreign anchors floors maps or worker ordinals", "[AIWorld][LivingFields][Arrival]")
{
    auto worker = LivingFields::Worker(901022);
    REQUIRE(worker);
    auto field = LivingFields::Field(worker->Field);
    REQUIRE(field);
    auto actualHome = field->DepotPoint;
    actualHome.Z = 0.0f;
    auto wrong = actualHome;
    wrong.MapId = 0;
    REQUIRE(LivingFields::ArrivalRadius(*worker, wrong, wrong) == Approx(3.0f));
    wrong = actualHome;
    wrong.X += 0.011f;
    REQUIRE(LivingFields::ArrivalRadius(*worker, wrong, wrong) == Approx(3.0f));
    wrong = actualHome;
    wrong.Y -= 0.011f;
    REQUIRE(LivingFields::ArrivalRadius(*worker, wrong, wrong) == Approx(3.0f));
    wrong = actualHome;
    wrong.Z = field->DepotPoint.Z + 0.6f;
    REQUIRE(LivingFields::ArrivalRadius(*worker, wrong, wrong) == Approx(3.0f));
    for (auto const& foreign : LivingFields::Fields)
        if (foreign.Id != field->Id)
        {
            auto foreignHome = foreign.DepotPoint;
            foreignHome.Z = 0.0f;
            REQUIRE(LivingFields::ArrivalRadius(*worker, foreignHome, foreignHome) == Approx(3.0f));
        }
    auto otherOrdinal = LivingFields::Worker(901023);
    REQUIRE(otherOrdinal);
    REQUIRE(LivingFields::ArrivalRadius(*otherOrdinal, actualHome, actualHome) == Approx(3.0f));
    auto forged = *worker;
    forged.Ordinal = 1;
    REQUIRE(LivingFields::ArrivalRadius(forged, actualHome, actualHome) == Approx(3.0f));
    // Exact own warehouse precision remains available independently of the
    // ordinary worker's home; it grants no foreign warehouse permission.
    REQUIRE(LivingFields::ArrivalRadius(*otherOrdinal, field->DepotPoint, actualHome) == Approx(1.9f));
}

TEST_CASE("Meal-only home rests preserve the farmer's unworked field point", "[AIWorld][LivingFields][RuntimePolicy]")
{
    auto ledger = FieldFixture(2);
    ActorRuntime actor;
    auto worker = LivingFields::Worker(901016);
    REQUIRE(worker);
    auto original = LivingFields::WorkPoint(*worker, actor.FieldCycle);
    REQUIRE(original);
    REQUIRE_FALSE(actor.FieldWorkedThisCycle);
    for (uint64 meal = 1; meal <= 2; ++meal)
    {
        actor.FarmingActivity = FieldActivity::Eat;
        actor.FarmingStage = FieldStage::Warehouse;
        REQUIRE(ledger.EatDepot(worker->Spawn, meal, worker->Depot, 1, true).Success);
        actor.FarmingActivity = FieldActivity::Rest;
        FinishFieldHomeRest(actor);
        REQUIRE(actor.FieldCycle == 0);
        REQUIRE_FALSE(actor.FieldWorkedThisCycle);
        REQUIRE(actor.FarmingStage == FieldStage::Field);
        REQUIRE(LivingFields::WorkPoint(*worker, actor.FieldCycle) == original);
    }
    REQUIRE(ledger.GetCounters().HarvestedTotal == 0);
    REQUIRE(ledger.GetCounters().ConsumedTotal == 2);
    REQUIRE(ledger.GetField(worker->Field)->Remaining == FieldCapacity);
    FieldConserved(ledger);
}

TEST_CASE("A completed field work visit authorizes one home-rest rotation through meals and interruptions", "[AIWorld][LivingFields][RuntimePolicy]")
{
    auto ledger = FieldFixture();
    ActorRuntime actor;
    actor.FieldCycle = 2;
    auto worker = LivingFields::Worker(901016);
    REQUIRE(worker);
    auto workedPoint = LivingFields::WorkPoint(*worker, actor.FieldCycle);
    REQUIRE(workedPoint);
    actor.FarmingActivity = FieldActivity::Work;
    REQUIRE_FALSE(ledger.HarvestField(worker->Spawn, 1, worker->Field, 1, 15100, false).Success);
    actor.FarmingActivity = FieldActivity::None;
    FinishFieldHomeRest(actor);
    REQUIRE(actor.FieldCycle == 2);
    REQUIRE_FALSE(actor.FieldWorkedThisCycle);
    REQUIRE(ledger.GetCounters().HarvestedTotal == 0);
    REQUIRE(ledger.HarvestField(worker->Spawn, 1, worker->Field, 1, 15100, true).Success);
    // Live runtime sets this receipt only after its owned 15-second work
    // completion. Clearing an interrupted activity must preserve that receipt.
    actor.FieldWorkedThisCycle = true;
    actor.FarmingActivity = FieldActivity::None;
    REQUIRE(actor.FieldWorkedThisCycle);
    REQUIRE(ledger.Deliver(worker->Spawn, 2, worker->Depot, 1, true).Success);
    REQUIRE(ledger.EatDepot(worker->Spawn, 3, worker->Depot, 1, true).Success);
    REQUIRE(actor.FieldWorkedThisCycle);
    actor.FarmingActivity = FieldActivity::Rest;
    FinishFieldHomeRest(actor);
    REQUIRE(actor.FieldCycle == 0);
    REQUIRE_FALSE(actor.FieldWorkedThisCycle);
    REQUIRE(actor.FarmingStage == FieldStage::Field);
    REQUIRE(LivingFields::WorkPoint(*worker, actor.FieldCycle) != workedPoint);
    auto nextPoint = LivingFields::WorkPoint(*worker, actor.FieldCycle);
    // A later rest without another completed work action cannot redeem the
    // previous visit a second time or skip this next assigned point.
    FinishFieldHomeRest(actor);
    REQUIRE(actor.FieldCycle == 0);
    REQUIRE(LivingFields::WorkPoint(*worker, actor.FieldCycle) == nextPoint);
    REQUIRE_FALSE(actor.FieldWorkedThisCycle);
    REQUIRE(ledger.GetCounters().HarvestedTotal == 1);
    REQUIRE(ledger.GetCounters().ConsumedTotal == 1);
    FieldConserved(ledger);
}

TEST_CASE("Completed maintenance or empty-field work rotates once without creating food", "[AIWorld][LivingFields][RuntimePolicy]")
{
    for (bool fullWarehouse : {false, true})
    {
        auto ledger = FieldFixture(fullWarehouse ? 30 : 0, fullWarehouse ? FieldCapacity : 0);
        ActorRuntime actor;
        actor.FieldCycle = 1;
        if (!fullWarehouse)
            REQUIRE_FALSE(ledger.HarvestField(901016, 1, 4, 1, 15100, true).Success);
        // Both paths completed the same owned work interval. Storage and
        // crop availability decide food production, independently of rotation.
        actor.FieldWorkedThisCycle = true;
        FinishFieldHomeRest(actor);
        REQUIRE(actor.FieldCycle == 2);
        REQUIRE_FALSE(actor.FieldWorkedThisCycle);
        REQUIRE(actor.FarmingStage == FieldStage::Field);
        REQUIRE(ledger.GetCounters().HarvestedTotal == 0);
        REQUIRE(ledger.GetCargo(901016) == 0);
        REQUIRE(ledger.GetField(4)->Remaining == (fullWarehouse ? FieldCapacity : 0));
        REQUIRE(ledger.GetDepot(4)->Stock == (fullWarehouse ? 30 : 0));
        FieldConserved(ledger);
    }
}
