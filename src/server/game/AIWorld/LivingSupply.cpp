/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "AIWorldMgr.h"
#include "Creature.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "PointMovementGenerator.h"
#include "Agent/LivingRecoveryPath.h"
#include "Scheduler/PlanningWorkBudget.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

namespace
{
    using Phase = LivingRoleState::Phase;
    using Activity = LivingRolePolicy::Activity;

    bool SupplyIdentity(AgentRecord const& record, bool consumersEnabled = false, bool raidsEnabled = false)
    {
        return record.MapId == 725 && record.Id.Value == record.SpawnId &&
            record.ControlMode == AgentControlMode::AIWorldControlled &&
            ((LivingSupply::Predator(record.SpawnId) && record.Type == AgentType::Predator) ||
                LivingSupply::HunterCapability(record.SpawnId, record.MapId, record.Type, record.ControlMode, true) ||
                LivingSupply::ConsumerCapability(record.SpawnId, record.MapId, record.Type, record.ControlMode, consumersEnabled) ||
                (raidsEnabled && ((record.SpawnId == LivingRaid::RaiderSpawn && record.Type == AgentType::Combatant) ||
                    (record.SpawnId == LivingRaid::GuardSpawn && record.Type == AgentType::Guard))));
    }

    bool OwnedGoal(AgentRecord const& record)
    {
        auto const& state = record.LivingRole;
        return record.ActiveGoalState && record.ActiveGoalState->Type == state.SourceGoal &&
            record.ActiveGoalState->StartedAtMs == state.StartedAtMs;
    }

    bool OwnedAction(AgentRecord const& record, ActionType type)
    {
        auto const& state = record.LivingRole;
        return OwnedGoal(record) && record.ActiveActionState && record.ActiveActionState->Type == type &&
            record.ActiveActionState->SourceGoal == state.SourceGoal &&
            record.ActiveActionState->GoalStartedAtMs == state.StartedAtMs &&
            record.ActiveActionState->StartedAtMs == state.StartedAtMs;
    }

    bool BoundActor(AgentRecord const& record, Creature const& creature)
    {
        return creature.GetMapId() == 725 && creature.GetSpawnId() == record.SpawnId &&
            record.WorldState == AgentWorldState::Materialized && record.RuntimeGuid == creature.GetGUID() &&
            record.LivingRole.RuntimeGuid == creature.GetGUID() && creature.IsAlive() &&
            creature.IsInWorld() && !creature.IsPet() && !creature.IsCharmed() &&
            creature.GetCharmerOrOwnerGUID().IsEmpty() && !creature.IsControlledByPlayer();
    }

    bool InSupplyScope(SimulationScope const& scope, Creature const& creature)
    {
        return scope.Contains(creature.GetMapId(), creature.GetZoneId(), creature.GetPositionX(),
            creature.GetPositionY(), creature.GetPositionZ()) && scope.ContainsActor(creature.GetMapId(), creature.GetSpawnId());
    }

    bool SupplyMovement(char const* purpose)
    {
        std::string_view value(purpose);
        return value == "TO_HUNTING_GROUND" || value == "SUPPLY_SEARCH" ||
            value == "DELIVER_FOOD" || value == "GET_DEPOT_FOOD" || value == "RETURN_DEPOT" ||
            value == "RETURN_CONSUMER_HOME";
    }
}

void AIWorldMgr::InitializeLivingSupply()
{
    _supplyLedger = {};
    _supplyActors.clear();
    _supplySources.clear();
    _raidRaider = {}; _raidGuard = {};
    _raidStartedAtMs = GetCurrentTimeMs();
    if (!_livingSupplyEnabled)
    {
        if (_livingConsumersEnabled)
            TC_LOG_ERROR("ai.world", "Living depot consumers require living supply; disabled");
        _livingConsumersEnabled = false;
        _livingRaidsEnabled = false;
        return;
    }
    auto const& scope = GetSimulationScope();
    bool valid = _enabled && _livingRolesEnabled && _livingRoleExtensionsEnabled &&
        _restrictAgentsToSimulationScope && scope.Valid() && scope.MapId == 725 && scope.Bounds &&
        scope.ZoneIds == std::vector<uint32>{4988} && scope.SpawnIds.size() == 100;
    for (uint64 spawn = 901000; valid && spawn < 901100; ++spawn)
        valid = scope.ContainsSpawn(spawn);
    if (!valid)
    {
        _livingSupplyEnabled = false;
        _livingConsumersEnabled = false;
        _livingRaidsEnabled = false;
        TC_LOG_ERROR("ai.world", "Living supply requires the explicit bounded map-725 authored population and enabled living roles/extensions; disabled");
        return;
    }
    for (auto const& spec : LivingSupply::Depots)
        valid = _supplyLedger.ConfigureDepot(spec.Id) && valid;
    for (auto const& spec : LivingSupply::Hunters)
    {
        valid = _supplyLedger.RegisterActor(spec.Spawn, spec.Depot) && valid;
        _supplyActors.emplace(spec.Spawn, LivingSupply::ActorRuntime{});
    }
    if (_livingConsumersEnabled)
        for (auto const& spec : LivingSupply::Consumers)
        {
            valid = _supplyLedger.RegisterActor(spec.Spawn, spec.Depot, 0, false, true) && valid;
            _supplyActors.emplace(spec.Spawn, LivingSupply::ActorRuntime{});
        }
    for (uint64 spawn : {901076ull, 901081ull, 901086ull, 901091ull})
    {
        valid = _supplyLedger.RegisterActor(spawn, 0, 0, true) && valid;
        _supplyActors.emplace(spawn, LivingSupply::ActorRuntime{});
    }
    if (_livingRaidsEnabled)
    {
        valid = _livingConsumersEnabled && _supplyLedger.ConfigureDepot(LivingRaid::CampDepot) &&
            _supplyLedger.RegisterRaider(LivingRaid::RaiderSpawn) &&
            _supplyLedger.RegisterActor(LivingRaid::GuardSpawn, LivingRaid::SourceDepot, 0, false, true) && valid;
        _supplyActors.emplace(LivingRaid::RaiderSpawn, LivingSupply::ActorRuntime{});
        _supplyActors.emplace(LivingRaid::GuardSpawn, LivingSupply::ActorRuntime{});
    }
    if (!valid)
    {
        _livingSupplyEnabled = false;
        _livingConsumersEnabled = false;
        _livingRaidsEnabled = false;
        _supplyLedger = {};
        _supplyActors.clear();
        TC_LOG_ERROR("ai.world", "Living supply could not register its fixed runtime depots and participants; disabled");
    }
}

bool AIWorldMgr::IsSupplyHunter(AgentRecord const& record) const
{
    return LivingSupply::HunterCapability(record.SpawnId, record.MapId, record.Type,
        record.ControlMode, _livingSupplyEnabled) && record.Id.Value == record.SpawnId &&
        GetSimulationScope().ContainsActor(record.MapId, record.SpawnId);
}

bool AIWorldMgr::IsSupplyConsumer(AgentRecord const& record) const
{
    return (LivingSupply::ConsumerCapability(record.SpawnId, record.MapId, record.Type,
        record.ControlMode, _livingSupplyEnabled && _livingConsumersEnabled) || IsRaidGuard(record)) && record.Id.Value == record.SpawnId &&
        GetSimulationScope().ContainsActor(record.MapId, record.SpawnId);
}

LivingSupply::ConsumerSpec const* AIWorldMgr::SupplyConsumerSpec(AgentRecord const& record) const
{
    static LivingSupply::ConsumerSpec const guard{LivingRaid::GuardSpawn, LivingRaid::SourceDepot, AgentType::Guard};
    return IsRaidGuard(record) ? &guard : LivingSupply::Consumer(record.SpawnId);
}

LivingRolePolicy::Role AIWorldMgr::ResolveLivingRole(AgentRecord const& record, Creature const& creature) const
{
    if (IsSupplyHunter(record))
        return LivingRolePolicy::Role::Hunter;
    if (IsSupplyRaider(record))
        return LivingRolePolicy::Role::Raider;
    bool service = creature.IsVendor() || creature.IsTrainer() || creature.IsTaxi() || creature.IsBanker() ||
        creature.IsInnkeeper() || creature.IsAuctioner() || creature.IsSpiritService() || creature.IsBattleMaster();
    return LivingRolePolicy::Resolve(record.Type, creature.GetEntry(), service);
}

void AIWorldMgr::ObserveLivingSupply(AgentRecord const& record, Creature const* creature, uint64 nowMs)
{
    if (!_livingSupplyEnabled || record.MapId != 725 || record.Id.Value != record.SpawnId ||
        record.ControlMode != AgentControlMode::AIWorldControlled ||
        !GetSimulationScope().ContainsActor(record.MapId, record.SpawnId))
        return;
    _supplyLedger.Expire(nowMs);
    bool matches = creature && creature->GetMapId() == record.MapId && creature->GetSpawnId() == record.SpawnId;
    if (LivingSupply::Prey(record.SpawnId) && record.Type == AgentType::Prey)
    {
        auto& source = _supplySources[record.SpawnId];
        if (!matches)
        {
            if (source.Life) _supplyLedger.CloseCarcass({record.SpawnId, source.Life});
            // Retain the old GUID, life and alive observation. Reloading the
            // same corpse cannot become a fresh six-portion carcass.
            return;
        }
        bool alive = creature->IsAlive();
        bool newGuid = source.Guid != creature->GetGUID();
        bool newLife = newGuid || (alive && !source.Alive);
        bool newDeath = !alive && (newGuid || source.Alive);
        if (newLife)
        {
            if (source.Life) _supplyLedger.CloseCarcass({record.SpawnId, source.Life});
            if (source.Life == std::numeric_limits<uint64>::max()) return;
            ++source.Life;
        }
        source.Guid = creature->GetGUID();
        source.Alive = alive;
        if (newDeath && source.Life)
        {
            uint64 ttl = std::max<uint64>(1000, uint64(creature->GetCorpseDelay()) * 1000);
            uint64 expires = ttl > std::numeric_limits<uint64>::max() - nowMs ?
                std::numeric_limits<uint64>::max() : nowMs + ttl;
            _supplyLedger.RegisterCarcass({record.SpawnId, source.Life}, nowMs, expires);
        }
        return;
    }
    if (!SupplyIdentity(record, _livingConsumersEnabled, _livingRaidsEnabled)) return;
    auto found = _supplyActors.find(record.SpawnId);
    if (found == _supplyActors.end()) return;
    auto& actor = found->second;
    bool live = matches && creature->IsAlive();
    if (!live || !actor.Live || actor.Guid != creature->GetGUID())
    {
        actor.Lost += _supplyLedger.DropCargo(record.SpawnId);
        actor.Claim.reset();
        actor.Corpse = {};
        actor.Trip = actor.AtGround = false;
        actor.ReturningToHome = false;
        actor.Travel = {};
        LivingSupply::ClearHunterSearch(actor);
        actor.EmptySearches = actor.SearchLeg = 0;
        actor.Live = live;
        actor.Phase = live ? "IDLE" : matches ? "ACTOR_DEAD" : "UNLOADED";
        actor.LastResult = live ? "MATERIALIZED" : matches ? "ACTOR_DEAD" : "UNLOADED";
        if (IsSupplyRaider(record) || IsRaidGuard(record))
        {
            auto& raid = IsSupplyRaider(record) ? _raidRaider : _raidGuard;
            if (!live && raid.Active && actor.Stolen == raid.StolenAtStart) ++raid.Prevented;
            raid.Active = raid.Returning = false; raid.Leg = 0;
            // A loaded corpse retains the observed attempt for honest death
            // diagnostics; unloading cannot create a new incarnation.
            if (matches) raid.Guid = creature->GetGUID();
            raid.NextAtMs = nowMs + (live ? 1000 : LivingRaid::CooldownMs);
            raid.Phase = actor.Phase;
            if (live || (raid.Outcome != "INTERCEPTED" && raid.Outcome != "CAUGHT_WITH_CARGO" &&
                raid.Outcome != "INTERRUPTED_COMBAT")) raid.Outcome = actor.LastResult;
        }
        if (live)
        {
            actor.Guid = creature->GetGUID();
            actor.NextAtMs = nowMs + 1000;
        }
        // EventSerial and the ledger's replay watermark survive this cleanup.
    }
}

void AIWorldMgr::CancelSupplyClaim(AgentRecord const& record)
{
    if (!_livingSupplyEnabled || !SupplyIdentity(record, _livingConsumersEnabled)) return;
    auto found = _supplyActors.find(record.SpawnId);
    if (found == _supplyActors.end()) return;
    if (found->second.Claim) _supplyLedger.Cancel(*found->second.Claim);
    found->second.Claim.reset();
}

bool AIWorldMgr::ClaimSupplyCarcass(AgentRecord const& record, Creature& creature, Creature& prey, uint64 nowMs)
{
    if (!_livingSupplyEnabled || !SupplyIdentity(record, _livingConsumersEnabled) || !BoundActor(record, creature) ||
        !InSupplyScope(GetSimulationScope(), creature) || !InSupplyScope(GetSimulationScope(), prey) ||
        prey.IsAlive() || prey.GetEntry() != 883 || !LivingSupply::Prey(prey.GetSpawnId()) ||
        !creature.IsWithinDistInMap(&prey, 5.0f) || !creature.IsWithinLOSInMap(&prey) ||
        !creature.IsStopped() || creature.IsInCombat())
        return false;
    auto preyRecord = _registry.FindBySpawn(prey.GetMapId(), prey.GetSpawnId());
    if (!preyRecord || preyRecord->Type != AgentType::Prey) return false;
    ObserveLivingSupply(*preyRecord, &prey, nowMs);
    auto source = _supplySources.find(prey.GetSpawnId());
    auto actor = _supplyActors.find(record.SpawnId);
    if (source == _supplySources.end() || actor == _supplyActors.end() || source->second.Guid != prey.GetGUID() ||
        !source->second.Life || source->second.Alive || actor->second.Guid != creature.GetGUID()) return false;
    LivingSupply::CarcassKey key{prey.GetSpawnId(), source->second.Life};
    bool hunter = IsSupplyHunter(record);
    auto token = _supplyLedger.Reserve(record.SpawnId, key,
        hunter ? LivingSupply::ClaimKind::Harvest : LivingSupply::ClaimKind::PredatorMeal,
        hunter ? LivingSupply::HunterCargoCapacity : LivingSupply::PredatorMealPortions, nowMs);
    if (!token) return false;
    actor->second.Claim = token;
    actor->second.Corpse = key;
    actor->second.Phase = hunter ? "HARVESTING" : "FEEDING";
    actor->second.LastResult = hunter ? "HARVEST_RESERVED" : "MEAL_RESERVED";
    return true;
}

uint32 AIWorldMgr::CommitSupplyCarcass(AgentRecord const& record, Creature& creature, Creature& prey, uint64 nowMs)
{
    auto found = _supplyActors.find(record.SpawnId);
    if (!_livingSupplyEnabled || !SupplyIdentity(record, _livingConsumersEnabled) || found == _supplyActors.end() ||
        !found->second.Claim || !BoundActor(record, creature)) return 0;
    auto& actor = found->second;
    auto const& state = record.LivingRole;
    auto source = _supplySources.find(prey.GetSpawnId());
    bool valid = state.CurrentPhase == Phase::Feeding && state.SourceGoal == GoalType::Feed &&
        InSupplyScope(GetSimulationScope(), creature) && InSupplyScope(GetSimulationScope(), prey) &&
        state.TargetGuid == prey.GetGUID() && OwnedAction(record, ActionType::Eat) &&
        record.ActiveActionState->Target && record.ActiveActionState->Target->Guid == prey.GetGUID() &&
        nowMs >= state.StartedAtMs && nowMs - state.StartedAtMs >= 5000 &&
        !prey.IsAlive() && prey.GetEntry() == 883 && LivingSupply::Prey(prey.GetSpawnId()) &&
        !creature.IsInCombat() && creature.IsStopped() && creature.IsWithinDistInMap(&prey, 5.0f) &&
        creature.IsWithinLOSInMap(&prey) && actor.Guid == creature.GetGUID() &&
        source != _supplySources.end() && source->second.Guid == prey.GetGUID() &&
        !source->second.Alive && actor.Corpse == LivingSupply::CarcassKey{prey.GetSpawnId(), source->second.Life};
    if (!valid)
    {
        CancelSupplyClaim(record);
        actor.LastResult = "HARVEST_INTERRUPTED";
        return 0;
    }
    auto result = _supplyLedger.Commit(*actor.Claim, nowMs);
    actor.Claim.reset();
    if (!result.Success || result.Duplicate)
    {
        actor.LastResult = result.Reason;
        return 0;
    }
    actor.Harvested += result.Portions;
    if (IsSupplyHunter(record))
    {
        actor.Trip = actor.AtGround = false;
        LivingSupply::ClearHunterSearch(actor);
        actor.Phase = "CARRYING";
        actor.LastResult = "HARVESTED";
        actor.NextAtMs = nowMs + 1000;
    }
    else
    {
        actor.Consumed += result.Portions;
        actor.Phase = "FED";
        actor.LastResult = "ATE_CARCASS";
    }
    return result.Portions;
}

bool AIWorldMgr::SupplyWantsHunt(AgentRecord const& record) const
{
    auto found = _supplyActors.find(record.SpawnId);
    return IsSupplyHunter(record) && found != _supplyActors.end() && found->second.Live &&
        found->second.Trip && found->second.AtGround && !_supplyLedger.GetCargo(record.SpawnId);
}

std::optional<LivingSupplyTelemetry> AIWorldMgr::DescribeLivingSupply(Creature const& creature, AgentRecord const& record) const
{
    if (!_livingSupplyEnabled || !SupplyIdentity(record, _livingConsumersEnabled, _livingRaidsEnabled) || creature.GetMapId() != 725 ||
        creature.GetSpawnId() != record.SpawnId || record.WorldState != AgentWorldState::Materialized ||
        record.RuntimeGuid != creature.GetGUID()) return std::nullopt;
    auto found = _supplyActors.find(record.SpawnId);
    if (found == _supplyActors.end() || found->second.Guid != creature.GetGUID()) return std::nullopt;
    auto const& actor = found->second;
    bool hunter = IsSupplyHunter(record);
    bool consumer = IsSupplyConsumer(record);
    bool raider = IsSupplyRaider(record);
    LivingSupplyTelemetry info;
    info.Enabled = true;
    info.Role = hunter ? "HUNTER" : raider ? "RAIDER" : consumer ? "CONSUMER" : "PREDATOR";
    info.Phase = actor.Phase;
    info.LastResult = actor.LastResult;
    if (creature.IsAlive() && record.LivingRole.CurrentPhase == Phase::Hunting) info.Phase = "HUNTING";
    if (creature.IsAlive() && record.LivingRole.CurrentPhase == Phase::Feeding) info.Phase = hunter ? "HARVESTING" : "FEEDING";
    info.Cargo = _supplyLedger.GetCargo(record.SpawnId);
    info.Capacity = hunter ? LivingSupply::HunterCargoCapacity : raider ? LivingRaid::Portions : 0;
    info.Harvested = actor.Harvested;
    info.Delivered = actor.Delivered;
    info.Consumed = actor.Consumed;
    info.Lost = actor.Lost;
    info.Stolen = actor.Stolen;
    if (auto carcass = _supplyLedger.GetCarcass(actor.Corpse))
    {
        auto source = _supplySources.find(actor.Corpse.Source);
        Creature* corpse = creature.GetMap()->GetCreatureBySpawnId(actor.Corpse.Source);
        if (source != _supplySources.end() && source->second.Life == actor.Corpse.Life &&
            !source->second.Alive && corpse && !corpse->IsAlive() && corpse->GetGUID() == source->second.Guid)
        {
            info.CarcassRemaining = carcass->Remaining;
            info.CarcassSpawnId = actor.Corpse.Source;
            info.CarcassLife = actor.Corpse.Life;
        }
    }
    if (hunter || consumer || raider)
    {
        uint32 depotId = hunter ? LivingSupply::Hunter(record.SpawnId)->Depot : raider ? LivingRaid::CampDepot : SupplyConsumerSpec(record)->Depot;
        info.DepotId = depotId;
        if (hunter) info.HuntingGround = LivingSupply::Hunter(record.SpawnId)->Ground;
        info.Depot = DescribeFoodDepot(depotId);
    }
    auto const& counts = _supplyLedger.GetCounters();
    FoodAccountingTelemetry accounting;
    accounting.InitialFood = counts.InitialFood; accounting.Harvested = counts.HarvestedTotal;
    accounting.InDepots = _supplyLedger.DepotFood(); accounting.InCargo = _supplyLedger.CargoFood();
    accounting.Consumed = counts.ConsumedTotal; accounting.Lost = counts.LostTotal;
    accounting.Conserved = _supplyLedger.Conserved();
    info.Accounting = accounting;
    return info;
}

std::optional<ActionPosition> AIWorldMgr::PlanLivingSupplyStep(AgentRecord& record, Creature& creature,
    ActionPosition const& destination, uint64 nowMs, std::optional<RecoveryMovement>& approved)
{
    using namespace LivingReturnPolicy;
    approved.reset();
    if ((!IsSupplyHunter(record) && !IsSupplyConsumer(record) && !IsSupplyRaider(record)) || !BoundActor(record, creature))
        return std::nullopt;
    auto found = _supplyActors.find(record.SpawnId);
    if (found == _supplyActors.end()) return std::nullopt;
    auto& travel = found->second.Travel;
    auto& state = record.LivingRole;
    auto const& scope = GetSimulationScope();
    ActionPosition from{creature.GetMapId(), creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ()};
    auto const homePosition = creature.GetHomePosition();
    ActionPosition home{from.MapId, homePosition.GetPositionX(), homePosition.GetPositionY(), homePosition.GetPositionZ()};
    float homeRadius = IsSupplyRaider(record) ? LivingRaid::RaiderHomeRadius : 384.0f;
    auto inBounds = [&](ActionPosition const& point)
    {
        return Finite(point) && point.MapId == from.MapId &&
            scope.Contains(point.MapId, creature.GetMap()->GetZoneId(creature.GetPhaseMask(), point.X, point.Y, point.Z),
                point.X, point.Y, point.Z) && std::hypot(point.X-home.X, point.Y-home.Y) <= homeRadius;
    };
    if (!InSupplyScope(scope, creature) || !inBounds(from) || !inBounds(home) || !inBounds(destination))
    {
        travel = {};
        state.Planning.ResumeWork(nowMs);
        return std::nullopt;
    }
    std::optional<ActionPosition> danger = nowMs < state.DangerUntilMs ?
        std::optional<ActionPosition>(state.DangerPosition) : std::nullopt;
    float clearance = LivingRolePolicy::SafetyRadius(record.Id.Value);
    uint32 capabilities = uint32(creature.CanWalk()) + uint32(creature.CanEnterWater()) * 2 + uint32(creature.IsFlying()) * 4;
    if (!travel.Matches(creature.GetGUID(), destination, home, danger, clearance, creature.GetPhaseMask(), capabilities) ||
        !travel.ObserveSource(from) || (travel.RetryAtMs && nowMs >= travel.RetryAtMs))
        travel.Begin(creature.GetGUID(), from, destination, home, danger, clearance, creature.GetPhaseMask(), capabilities);
    state.Planning.ResumeWork(nowMs);
    if (travel.RetryAtMs) return std::nullopt;
    auto noRoute = [&]() -> std::optional<ActionPosition>
    {
        // Exhaustion is a bounded observation, not a permanent verdict about
        // mutable geometry. Recheck after the runtime's existing slow retry.
        travel.RetryAtMs = nowMs + 10000;
        return std::nullopt;
    };
    auto defer = [&]() -> std::optional<ActionPosition>
    {
        state.ReturnDiagnostics.Deferred = true;
        state.Planning.MarkDeferred(nowMs, "WORK_BUDGET", "SUPPLY_CORRIDOR");
        return std::nullopt;
    };
    // HomeCorridor's providers own their own leaf permits. Holding an outer
    // permit would prevent a suspended graph from making any progress.
    for (unsigned attempt = 0; attempt < 24; ++attempt)
    {
        if (travel.Route.Planned.empty())
        {
            auto* rememberedDanger = danger ? &*danger : nullptr;
            if (!travel.Search.Matches(from, destination, 3.0f, 384.0f, rememberedDanger, clearance, true))
                travel.Search.Begin(from, destination, 3.0f, 384.0f, rememberedDanger, clearance, true);
            // The caller's real arrival predicate includes 3D distance and
            // LOS. Being near an endpoint in XY cannot complete this trip.
            travel.Search.ArrivalChecked = true;
            uint64 started = PlanningWorkBudget::StartedOperations();
            auto corridor = LivingRecoveryPath::HomeCorridor(creature, from, destination, 3.0f, 384.0f,
                rememberedDanger, clearance, &state.ReturnDiagnostics, &travel.Search, true);
            if (PlanningWorkBudget::StartedOperations() != started) state.Planning.MarkProgress(nowMs);
            if (state.ReturnDiagnostics.Deferred) return defer();
            if (corridor.empty()) return noRoute();
            travel.Route.Planned = std::move(corridor);
            travel.Route.SurfaceCorridor = travel.Search.Report.SurfaceCorridor;
            if (IsSupplyRaider(record) && !travel.Route.SurfaceCorridor)
            {
                auto straight = LivingRaid::CoalesceStraightLegs(from, travel.Route.Planned);
                if (!straight.empty()) travel.Route.Planned = std::move(straight);
            }
        }
        // Remove only the literal origin. Nearby future bends remain in the
        // proof and are consumed individually after an owned arrival.
        while (!travel.Route.Planned.empty() && Distance(from, travel.Route.Planned.front()) <= 0.01f)
            travel.Route.Planned.erase(travel.Route.Planned.begin());
        if (travel.Route.Planned.empty()) continue;
        ActionPosition query = travel.Route.Planned.front();
        if (!UsefulStep(from, query) || travel.Route.Failed(from, query))
        {
            travel.RejectCandidate(from, query);
            continue;
        }
        auto permit = PlanningWorkBudget::TryAcquire();
        if (!permit) return defer();
        state.Planning.MarkProgress(nowMs);
        auto const& endpoint = travel.Route.Planned.back();
        float lift = std::max(0.5f, creature.GetCollisionHeight());
        bool endpointReachable = Distance(endpoint, destination) <= 3.0f &&
            creature.GetMap()->isInLineOfSight(endpoint.X, endpoint.Y, endpoint.Z + lift,
                destination.X, destination.Y, destination.Z + lift, creature.GetPhaseMask(),
                LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing);
        if (!endpointReachable || !std::all_of(travel.Route.Planned.begin(), travel.Route.Planned.end(), inBounds))
        {
            travel.RejectCandidate(from, query);
            continue;
        }
        RecoveryMovement request{query, home, homeRadius, danger, false};
        request.SurfaceCorridor = travel.Route.SurfaceCorridor;
        request.DangerRadius = clearance;
        Movement::PointsArray points;
        state.ReturnDiagnostics.Deferred = false;
        if (!LivingRecoveryPath::Build(creature, request, points, &state.ReturnDiagnostics))
        {
            travel.RejectCandidate(from, query);
            continue;
        }
        auto const& end = points.back();
        request.Destination = {from.MapId, end.x, end.y, end.z};
        request.QueryDestination = query;
        travel.Pending = request;
        travel.PendingFrom = from;
        approved = request;
        return request.Destination;
    }
    // Bounded administrative work may yield even before the next expensive
    // query. Keep its candidate cursor rather than declaring a route failure.
    return defer();
}

bool AIWorldMgr::UpdateLivingSupplyHunter(AgentRecord& record, Creature& creature, uint64 nowMs,
    std::function<bool(ActionRequest, Phase, Unit*, bool)> const& start,
    std::function<std::optional<ActionPosition>(ActionPosition const&)> const& step, bool searchOnly)
{
    if (!IsSupplyHunter(record) || !BoundActor(record, creature)) return false;
    auto found = _supplyActors.find(record.SpawnId);
    if (found == _supplyActors.end()) return false;
    auto& actor = found->second;
    auto& state = record.LivingRole;
    auto spec = LivingSupply::Hunter(record.SpawnId);
    auto location = LivingSupply::DepotLocation(spec->Depot);
    auto depot = _supplyLedger.GetDepot(spec->Depot);
    if (!location || !depot) return true;
    if (state.CurrentPhase == Phase::Hunting || state.CurrentPhase == Phase::Feeding) return false;

    auto finish = [&](char const* result, uint64 delay)
    {
        StopLivingRole(record, creature);
        actor.LastResult = result;
        actor.Phase = "WAITING";
        actor.NextAtMs = nowMs + delay;
        state.NextDecisionAtMs = actor.NextAtMs;
    };
    if (state.CurrentPhase == Phase::Moving && SupplyMovement(state.MovementPurpose))
    {
        auto* point = dynamic_cast<PointMovementGenerator<Creature>*>(creature.GetMotionMaster()->GetCurrentMovementGenerator(MOTION_SLOT_ACTIVE));
        bool ownership = state.SourceGoal == GoalType::LocalActivity && OwnedGoal(record) &&
            (!record.ActiveActionState || OwnedAction(record, ActionType::MoveTo));
        bool arrived = ownership && creature.IsStopped() &&
            creature.GetExactDist(state.Destination.X, state.Destination.Y, state.Destination.Z) <= 2.0f;
        if (!arrived && ownership && record.ActiveActionState && point && point->GetId() == ActionExecutor::MovePointId &&
            state.MoveWatchdog.Continue(nowMs, {725, creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ()}, false))
            return true;
        ActionPosition reached{725, creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ()};
        if (arrived && actor.Travel.Pending && RecoveryMovement::SamePoint(actor.Travel.Pending->Destination, state.Destination))
        {
            if (!actor.Travel.ObserveSource(reached, true)) actor.Travel = {};
        }
        else actor.Travel.RejectPending();
        finish(arrived ? "ARRIVED" : "MOVE_FAILED", arrived ? 1000 : 10000);
        if (!arrived) actor.Phase = "NO_ROUTE";
        return true;
    }

    bool delivering = std::string_view(actor.Phase) == "DELIVERING";
    bool eating = std::string_view(actor.Phase) == "EATING_DEPOT";
    if (state.CurrentPhase == Phase::Acting && (delivering || eating))
    {
        bool owned = state.SourceGoal == GoalType::LocalActivity && OwnedAction(record, ActionType::Ambient) &&
            state.Activity == (delivering ? Activity::Work : Activity::Eat) && creature.IsStopped() &&
            !creature.IsInCombat() && creature.GetStandState() == UNIT_STAND_STATE_STAND &&
            creature.GetExactDist(location->Point.X, location->Point.Y, location->Point.Z) <= 3.0f &&
            creature.IsWithinLOS(location->Point.X, location->Point.Y, location->Point.Z);
        if (!owned || nowMs < state.StartedAtMs)
        {
            finish("ACTIVITY_INTERRUPTED", 10000);
            return true;
        }
        if (nowMs - state.StartedAtMs < 5000) return true;
        if (actor.EventSerial == std::numeric_limits<uint64>::max())
        {
            finish("EVENT_ID_EXHAUSTED", 60000);
            return true;
        }
        LivingSupply::Result result = delivering ?
            _supplyLedger.Deliver(record.SpawnId, ++actor.EventSerial, spec->Depot, LivingSupply::HunterCargoCapacity, true) :
            _supplyLedger.EatDepot(record.SpawnId, ++actor.EventSerial, spec->Depot, 1, true);
        if (result.Success && !result.Duplicate)
        {
            if (delivering) actor.Delivered += result.Portions;
            else { actor.Consumed += result.Portions; _needsSystem.SatisfyHunger(record.Needs); }
        }
        finish(result.Success ? delivering ? "DELIVERED" : "ATE_DEPOT" : result.Reason, 2000);
        return true;
    }
    if (state.CurrentPhase == Phase::Acting) return false;
    if (state.CurrentPhase != Phase::Idle) return false;
    if (nowMs < actor.NextAtMs || (!searchOnly && nowMs < state.NextDecisionAtMs)) return true;

    auto travel = [&](ActionPosition const& destination, char const* phase, char const* purpose, bool searchAttempt = false)
    {
        auto next = step(destination);
        if (!next && state.Planning.Deferred)
        {
            actor.Phase = "WAITING"; actor.LastResult = "PLANNING_DEFERRED";
            actor.NextAtMs = nowMs + 100;
            state.NextDecisionAtMs = nowMs + 100;
            state.MovementPurpose = "SUPPLY_PLANNING_DEFERRED";
            return true;
        }
        if (searchAttempt) LivingSupply::FinishHunterSearchAttempt(actor, true);
        if (!next)
        {
            actor.Phase = "NO_ROUTE"; actor.LastResult = "NO_ROUTE";
            actor.NextAtMs = nowMs + 10000;
            return true;
        }
        ActionRequest move;
        move.Type = ActionType::MoveTo; move.SourceGoal = GoalType::LocalActivity; move.Destination = next;
        actor.LastResult = "MOVE_REJECTED";
        if (!start(move, Phase::Moving, nullptr, false))
        {
            if (actor.LastResult != "MOVEMENT_OWNER_BUSY" && actor.LastResult != "ACTOR_MOVEMENT_BUSY" &&
                actor.LastResult != "ACTOR_IN_COMBAT") actor.Travel.RejectPending();
            actor.Phase = "NO_ROUTE";
            actor.NextAtMs = nowMs + 10000;
            return true;
        }
        state.MovementPurpose = purpose;
        actor.Phase = phase;
        actor.LastResult = "MOVING";
        return true;
    };
    auto depotActivity = [&](Activity activity, char const* phase)
    {
        ActionRequest request;
        request.Type = ActionType::Ambient; request.SourceGoal = GoalType::LocalActivity; request.AmbientActivity = activity;
        if (start(request, Phase::Acting, nullptr, false))
        { actor.Phase = phase; actor.LastResult = "STARTED"; }
        else
        { actor.Phase = "WAITING"; actor.LastResult = "ACTIVITY_REJECTED"; actor.NextAtMs = nowMs + 10000; }
        return true;
    };
    float depotDistance = creature.GetExactDist(location->Point.X, location->Point.Y, location->Point.Z);
    bool atDepot = depotDistance <= 3.0f && creature.IsWithinLOS(location->Point.X, location->Point.Y, location->Point.Z);
    if (_supplyLedger.GetCargo(record.SpawnId))
    {
        actor.Trip = actor.AtGround = false;
        LivingSupply::ClearHunterSearch(actor);
        if (!atDepot) return travel(location->Point, "RETURNING_TO_DEPOT", "DELIVER_FOOD");
        if (depot->Stock >= depot->Capacity)
        {
            if (record.Needs.Hunger >= 0.65f) return depotActivity(Activity::Eat, "EATING_DEPOT");
            actor.Phase = "WAITING"; actor.LastResult = "DEPOT_FULL"; actor.NextAtMs = nowMs + 10000;
            return true;
        }
        return depotActivity(Activity::Work, "DELIVERING");
    }
    if (record.Needs.Hunger >= 0.65f && depot->Stock)
    {
        actor.Trip = actor.AtGround = false;
        LivingSupply::ClearHunterSearch(actor);
        if (!atDepot) return travel(location->Point, "RETURNING_TO_DEPOT", "GET_DEPOT_FOOD");
        return depotActivity(Activity::Eat, "EATING_DEPOT");
    }
    if (!actor.Trip && (depot->Stock >= depot->Target || actor.EmptySearches >= 8))
    {
        actor.AtGround = false;
        LivingSupply::ClearHunterSearch(actor);
        if (!atDepot) return travel(location->Point, "RETURNING_TO_DEPOT", "RETURN_DEPOT");
        bool empty = actor.EmptySearches >= 8;
        actor.EmptySearches = 0;
        actor.NextAtMs = nowMs + (empty ? 60000 : 20000);
        return depotActivity(Activity::Rest, "RESTING");
    }
    if (!actor.Trip)
    {
        actor.Trip = true; actor.AtGround = false;
        actor.EmptySearches = actor.SearchLeg = 0;
        LivingSupply::ClearHunterSearch(actor);
    }
    float groundDistance = creature.GetExactDist2d(spec->Ground.X, spec->Ground.Y);
    if (groundDistance <= 8.0f) actor.AtGround = true;
    if (actor.AtGround && groundDistance > 48.0f) actor.AtGround = false;
    if (!actor.AtGround) return travel(spec->Ground, "TO_HUNTING_GROUND", "TO_HUNTING_GROUND");
    if (!searchOnly)
    { actor.Phase = "SEARCHING"; return false; }
    if (actor.EmptySearches >= 8)
    {
        actor.Trip = actor.AtGround = false;
        actor.LastResult = "NO_PREY";
        return travel(location->Point, "RETURNING_TO_DEPOT", "RETURN_DEPOT");
    }
    ActionPosition search = LivingSupply::PrepareHunterSearch(actor, spec->Ground);
    return travel(search, "SEARCHING", "SUPPLY_SEARCH", true);
}

bool AIWorldMgr::UpdateLivingSupplyConsumer(AgentRecord& record, Creature& creature, uint64 nowMs,
    std::function<bool(ActionRequest, Phase, Unit*, bool)> const& start,
    std::function<std::optional<ActionPosition>(ActionPosition const&)> const& step)
{
    if (!IsSupplyConsumer(record) || !BoundActor(record, creature)) return false;
    auto found = _supplyActors.find(record.SpawnId);
    auto spec = SupplyConsumerSpec(record);
    if (found == _supplyActors.end() || !spec) return false;
    auto& actor = found->second;
    auto& state = record.LivingRole;
    auto location = LivingSupply::DepotLocation(spec->Depot);
    auto depot = _supplyLedger.GetDepot(spec->Depot);
    if (!location || !depot) return true;

    auto finish = [&](char const* phase, char const* result, uint64 delay)
    {
        StopLivingRole(record, creature);
        actor.Phase = phase;
        actor.LastResult = result;
        actor.NextAtMs = nowMs + delay;
        state.NextDecisionAtMs = actor.NextAtMs;
    };
    bool hungry = record.Needs.Hunger >= 0.65f;
    bool supplyMove = std::string_view(state.MovementPurpose) == "GET_DEPOT_FOOD" ||
        std::string_view(state.MovementPurpose) == "RETURN_CONSUMER_HOME";
    if (state.CurrentPhase == Phase::Moving && supplyMove)
    {
        auto* point = dynamic_cast<PointMovementGenerator<Creature>*>(creature.GetMotionMaster()->GetCurrentMovementGenerator(MOTION_SLOT_ACTIVE));
        bool ownership = state.SourceGoal == GoalType::LocalActivity && OwnedGoal(record) &&
            (!record.ActiveActionState || OwnedAction(record, ActionType::MoveTo));
        bool arrived = ownership && creature.IsStopped() &&
            creature.GetExactDist(state.Destination.X, state.Destination.Y, state.Destination.Z) <= 2.0f;
        if (!arrived && ownership && record.ActiveActionState && point && point->GetId() == ActionExecutor::MovePointId &&
            state.MoveWatchdog.Continue(nowMs, {725, creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ()}, false))
            return true;
        ActionPosition reached{725, creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ()};
        if (arrived && actor.Travel.Pending && RecoveryMovement::SamePoint(actor.Travel.Pending->Destination, state.Destination))
        {
            if (!actor.Travel.ObserveSource(reached, true)) actor.Travel = {};
        }
        else actor.Travel.RejectPending();
        finish(arrived ? "WAITING" : "NO_ROUTE", arrived ? "ARRIVED" : "MOVE_FAILED", arrived ? 1000 : 10000);
        return true;
    }
    if (state.CurrentPhase == Phase::Moving && hungry && state.SourceGoal == GoalType::LocalActivity &&
        OwnedAction(record, ActionType::MoveTo))
    {
        // A need may interrupt this actor's own ordinary stroll. Foreign or
        // emergency motion remains with its existing owner.
        StopLivingRole(record, creature);
        state.NextDecisionAtMs = nowMs;
    }
    if (state.CurrentPhase == Phase::Acting && std::string_view(actor.Phase) == "EATING_DEPOT")
    {
        bool owned = state.SourceGoal == GoalType::LocalActivity && OwnedAction(record, ActionType::Ambient) &&
            state.Activity == Activity::Eat && creature.IsStopped() && !creature.IsInCombat() &&
            creature.GetStandState() == UNIT_STAND_STATE_STAND &&
            creature.GetExactDist(location->Point.X, location->Point.Y, location->Point.Z) <= 3.0f &&
            creature.IsWithinLOS(location->Point.X, location->Point.Y, location->Point.Z);
        if (!owned || nowMs < state.StartedAtMs)
        {
            finish("WAITING", "ACTIVITY_INTERRUPTED", 10000);
            return true;
        }
        if (nowMs - state.StartedAtMs < 5000) return true;
        if (actor.EventSerial == std::numeric_limits<uint64>::max())
        {
            finish("WAITING", "EVENT_ID_EXHAUSTED", 60000);
            return true;
        }
        auto result = _supplyLedger.EatDepot(record.SpawnId, ++actor.EventSerial, spec->Depot, 1, true);
        if (result.Success && !result.Duplicate)
        {
            actor.Consumed += result.Portions;
            _needsSystem.SatisfyHunger(record.Needs);
            actor.ReturningToHome = true;
            finish("RETURNING_HOME", "ATE_DEPOT", 2000);
        }
        else
            finish("NO_FOOD", result.Reason, 15000);
        return true;
    }
    if (state.CurrentPhase != Phase::Idle) return false;
    if (!hungry && !actor.ReturningToHome)
    {
        actor.Phase = "ORDINARY";
        return false;
    }
    if (nowMs < actor.NextAtMs || nowMs < state.NextDecisionAtMs) return true;

    auto travel = [&](ActionPosition const& destination, char const* phase, char const* purpose)
    {
        auto next = step(destination);
        if (!next && state.Planning.Deferred)
        {
            actor.Phase = "WAITING"; actor.LastResult = "PLANNING_DEFERRED";
            actor.NextAtMs = nowMs + 100;
            state.NextDecisionAtMs = nowMs + 100;
            state.MovementPurpose = "SUPPLY_PLANNING_DEFERRED";
            return true;
        }
        if (!next)
        {
            actor.Phase = "NO_ROUTE"; actor.LastResult = "NO_ROUTE";
            actor.NextAtMs = nowMs + 10000;
            return true;
        }
        ActionRequest move;
        move.Type = ActionType::MoveTo; move.SourceGoal = GoalType::LocalActivity; move.Destination = next;
        actor.LastResult = "MOVE_REJECTED";
        if (!start(move, Phase::Moving, nullptr, false))
        {
            if (actor.LastResult != "MOVEMENT_OWNER_BUSY" && actor.LastResult != "ACTOR_MOVEMENT_BUSY" &&
                actor.LastResult != "ACTOR_IN_COMBAT") actor.Travel.RejectPending();
            actor.Phase = "NO_ROUTE";
            actor.NextAtMs = nowMs + 10000;
            return true;
        }
        state.MovementPurpose = purpose;
        actor.Phase = phase;
        actor.LastResult = "MOVING";
        return true;
    };
    if (actor.ReturningToHome)
    {
        auto const home = creature.GetHomePosition();
        ActionPosition destination{725, home.GetPositionX(), home.GetPositionY(), home.GetPositionZ()};
        if (creature.GetExactDist(destination.X, destination.Y, destination.Z) > 3.0f ||
            !creature.IsWithinLOS(destination.X, destination.Y, destination.Z))
            return travel(destination, "RETURNING_HOME", "RETURN_CONSUMER_HOME");
        actor.ReturningToHome = false;
        actor.Phase = "ORDINARY";
        actor.LastResult = "HOME_REACHED";
        return false;
    }
    if (!depot->Stock)
    {
        actor.Phase = "NO_FOOD";
        actor.LastResult = "NO_FOOD";
        actor.NextAtMs = nowMs + 15000;
        return true;
    }
    if (creature.GetExactDist(location->Point.X, location->Point.Y, location->Point.Z) > 3.0f ||
        !creature.IsWithinLOS(location->Point.X, location->Point.Y, location->Point.Z))
        return travel(location->Point, "TO_DEPOT", "GET_DEPOT_FOOD");
    ActionRequest eat;
    eat.Type = ActionType::Ambient; eat.SourceGoal = GoalType::LocalActivity; eat.AmbientActivity = Activity::Eat;
    if (start(eat, Phase::Acting, nullptr, false))
    {
        actor.Phase = "EATING_DEPOT";
        actor.LastResult = "STARTED";
    }
    else
    {
        actor.Phase = "WAITING"; actor.LastResult = "MEAL_REJECTED";
        actor.NextAtMs = nowMs + 10000;
    }
    return true;
}
