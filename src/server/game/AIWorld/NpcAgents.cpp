/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "AIWorldMgr.h"
#include "Agent/LivingRecoveryPath.h"
#include "Agent/NpcAgentChat.h"
#include "Inference/NpcAgentJsonCodec.h"
#include "Config.h"
#include "Creature.h"
#include "Log.h"
#include "Map.h"
#include "MapManager.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PointMovementGenerator.h"
#include "SharedDefines.h"
#include "SpellAuraDefines.h"
#include "ThreatManager.h"
#include "WorldPacket.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>

namespace
{
    using Phase = LivingRoleState::Phase;
    using Activity = LivingRolePolicy::Activity;
    bool OwnNpcAction(AgentRecord const& record, NpcAgentRuntime const& state)
    {
        return state.ActionAt && state.Guid == record.RuntimeGuid && record.LivingRole.RuntimeGuid == state.Guid &&
            record.LivingRole.StartedAtMs == state.ActionAt &&
            record.ActiveGoalState && record.ActiveGoalState->StartedAtMs == state.ActionAt &&
            record.ActiveActionState && record.ActiveActionState->StartedAtMs == state.ActionAt &&
            record.ActiveActionState->GoalStartedAtMs == state.ActionAt &&
            record.ActiveActionState->SourceGoal == record.ActiveGoalState->Type &&
            record.LivingRole.SourceGoal == record.ActiveGoalState->Type;
    }
    bool NpcAvailable(Creature const& creature, SimulationScope const& scope)
    {
        return creature.IsAlive() && creature.IsInWorld() && !creature.IsPet() && !creature.IsCharmed() &&
            !creature.IsControlledByPlayer() && creature.GetCharmerOrOwnerGUID().IsEmpty() &&
            scope.ContainsActor(creature.GetMapId(), creature.GetSpawnId()) &&
            scope.Contains(creature.GetMapId(), creature.GetZoneId(),
                creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ());
    }
    bool VisiblePartner(Creature const& actor, Unit const& partner)
    {
        return actor.IsInWorld() && actor.IsAlive() && partner.IsInWorld() && partner.IsAlive() && actor.GetMapId() == partner.GetMapId() &&
            actor.IsWithinDistInMap(&partner, 25.0f) && actor.CanSeeOrDetect(&partner) && actor.IsWithinLOSInMap(&partner);
    }
    Unit* NpcThreat(Creature& actor)
    {
        Unit* threat = actor.GetThreatManager().GetCurrentVictim();
        return threat && threat->IsAlive() && VisiblePartner(actor, *threat) && actor.IsInCombatWith(threat) ? threat : nullptr;
    }
    bool OwnPoint(Creature& creature)
    {
        auto* point = dynamic_cast<PointMovementGenerator<Creature>*>(
            creature.GetMotionMaster()->GetCurrentMovementGenerator(MOTION_SLOT_ACTIVE));
        return point && point->GetId() == ActionExecutor::MovePointId;
    }
    void ExpireNpcSpeechTurns(NpcAgentRuntime& state, uint64 now)
    {
        bool connected = !state.Speech.PlayerTurn || ObjectAccessor::FindConnectedPlayer(state.Speech.PlayerTurn->Partner);
        if (state.Speech.Expire(now, connected))
        { state.Conversation = 0; state.Hop = 0; state.NextThinkAt = std::min(state.NextThinkAt, now); }
    }
    uint64 NpcSpeechScene(NpcAgentRuntime const& state, NpcAgentContext const& context)
    {
        uint64 hash = 1469598103934665603ull;
        auto mix = [&](uint64 value) { hash ^= value; hash *= 1099511628211ull; };
        // A new visible audience, another part of the scene or a substantial
        // need change can justify an invitation to speak. Ordinary ticks cannot.
        mix(uint64(int64(std::floor(context.Self.X / 16.0f))));
        mix(uint64(int64(std::floor(context.Self.Y / 16.0f))));
        for (float need : {context.Needs.HealthPressure, context.Needs.Hunger, context.Needs.Fatigue,
            context.Needs.SafetyPressure, context.Needs.ResourcePressure})
            mix(std::isfinite(need) ? uint64(std::clamp(need, 0.0f, 1.0f) * 4.0f) : 0);
        std::vector<uint64> audience;
        for (auto const& [token, recipient] : state.Recipients) { (void)token; audience.push_back(recipient.Guid.GetRawValue()); }
        std::sort(audience.begin(), audience.end());
        for (uint64 guid : audience) mix(guid);
        return hash;
    }
}

bool AIWorldMgr::IsNpcAgent(AgentRecord const& record) const
{
    auto const* profile = _npcAgentEnabled ? _npcAgentProfiles.Get(record.Id) : nullptr;
    auto participation = _spawnParticipationCatalog.Resolve(record.SpawnId);
    return profile && profile->MapId == record.MapId && profile->SpawnId == record.SpawnId &&
        record.ControlMode == AgentControlMode::AIWorldControlled && InSimulationMembership(record.MapId, record.SpawnId) &&
        (participation == SpawnParticipationMode::FullAgent || participation == SpawnParticipationMode::LightweightBackground);
}

void AIWorldMgr::InitializeNpcAgents()
{
    if (!_npcAgentEnabled) return;
    if (!_enabled || !_livingRolesEnabled || !_livingRoleExtensionsEnabled || !_restrictAgentsToSimulationScope)
        throw std::runtime_error("NPC agents require enabled scoped AIWorld and living role action capabilities");
    std::string profilesDirectory = sConfigMgr->GetStringDefault("AIWorld.NpcAgentProfilesDirectory", "data/npc_agents");
    if (!_npcAgentProfiles.Load(profilesDirectory) ||
        !_npcAgentProfiles.Size() || _npcAgentProfiles.Size() > 16)
        throw std::runtime_error("NPC agent profiles must be a valid directory containing 1..16 individual profiles");
    std::size_t accepted = 0;
    for (AgentId id : _registry.GetAgents())
        if (auto* record = _registry.Find(id); record && _npcAgentProfiles.Get(id))
        {
            if (!IsNpcAgent(*record) || !LivingRolePolicy::KnownRole(LivingRolePolicy::Resolve(record->Type, 0, false)))
                throw std::runtime_error("NPC profile must match an existing controlled participating agent");
            ++accepted;
        }
    if (accepted != _npcAgentProfiles.Size())
        throw std::runtime_error("NPC profile references an unregistered or mismatched agent");
    std::string appearanceDirectory = sConfigMgr->GetStringDefault("AIWorld.NpcAgentAppearancesDirectory",
        (std::filesystem::path(profilesDirectory).parent_path() / "npc_appearances").string());
    if (!_npcAgentAppearances.Load(appearanceDirectory))
        throw std::runtime_error("NPC appearance directory contains an invalid sidecar");
    for (auto const& [spawn, appearance] : _npcAgentAppearances.Entries())
    {
        auto const* profile = _npcAgentProfiles.Get(AgentId{spawn});
        if (!profile || profile->MapId != appearance.MapId || profile->SpawnId != spawn ||
            !ValidateNativeNpcAgentAppearance(appearance))
            throw std::runtime_error("NPC appearance must match an opted-in profile and valid native client data");
    }
    TC_LOG_INFO("ai.world", "LLM NPC controller enabled: profiles={} appearances={} intervalMs={} maxInFlight={}",
        accepted, _npcAgentAppearances.Size(), _npcAgentIntervalMs, _npcAgentMaxInFlight);
}

bool AIWorldMgr::SendNpcAgentAppearance(Player& observer, Creature const& creature) const
{
    if (!_npcAgentEnabled || !observer.IsInWorld() || !creature.IsInWorld() || observer.GetMap() != creature.GetMap() ||
        !observer.HaveAtClient(&creature) || !observer.CanSeeOrDetect(&creature)) return false;
    auto const* appearance = _npcAgentAppearances.Get(creature.GetSpawnId());
    auto const* record = _registry.FindBySpawn(creature.GetMapId(), creature.GetSpawnId());
    if (!appearance || appearance->MapId != creature.GetMapId() || !record || !IsNpcAgent(*record) ||
        record->RuntimeGuid != creature.GetGUID() || !creature.HasUnitFlag2(UNIT_FLAG2_MIRROR_IMAGE)) return false;
    if (creature.HasAuraType(SPELL_AURA_TRANSFORM) || creature.HasAuraType(SPELL_AURA_MOD_SHAPESHIFT) ||
        creature.HasAuraType(SPELL_AURA_CLONE_CASTER)) return false;
    uint32 display = NativeNpcAgentAppearanceDisplay(*appearance);
    if (creature.GetDisplayId() != display) return false; // respect another active transformation
    WorldPacket packet;
    if (!BuildNpcAgentMirrorImageData(packet, creature.GetGUID(), display, *appearance)) return false;
    observer.SendDirectMessage(&packet);
    return true;
}

void AIWorldMgr::HandleNpcPlayerSpeech(Player* player, std::string const& text, uint32 language)
{
    if (!player || language == LANG_ADDON || player->GetTarget().IsEmpty()) return;
    std::string clean = CleanNpcAgentSpeechText(text);
    if (clean.empty()) return;
    NpcAgentPlayerSpeech input{player->GetGUID(), player->GetTarget(), player->GetMapId(), language, std::move(clean)};
    std::lock_guard<std::mutex> lock(_npcAgentSpeechMutex);
    if (_npcAgentPlayerSpeech.size() < 128) _npcAgentPlayerSpeech.push_back(std::move(input));
}

void AIWorldMgr::DrainNpcPlayerSpeech()
{
    std::deque<NpcAgentPlayerSpeech> inputs;
    { std::lock_guard<std::mutex> lock(_npcAgentSpeechMutex); inputs.swap(_npcAgentPlayerSpeech); }
    if (!_npcAgentEnabled) return;
    uint64 now = GetCurrentTimeMs();
    for (auto const& input : inputs)
    {
        Player* player = ObjectAccessor::FindConnectedPlayer(input.Player);
        if (!player || !player->IsInWorld() || player->GetMapId() != input.Map || !player->IsAlive()) continue;
        Unit* selected = ObjectAccessor::GetUnit(*player, input.Target);
        Creature* npc = selected ? selected->ToCreature() : nullptr;
        if (!npc || !VisiblePartner(*npc, *player) || !player->CanSeeOrDetect(npc)) continue;
        AgentRecord* record = _registry.FindBySpawn(npc->GetMapId(), npc->GetSpawnId());
        if (!record || !IsNpcAgent(*record) || !NpcAvailable(*npc, GetSimulationScope())) continue;
        _registry.BindCreature(record->Id, *npc);
        auto& state = _npcAgentStates[record->Id.Value];
        if (state.Guid != npc->GetGUID()) { state = {}; state.Guid = npc->GetGUID(); }
        if (!record->LivingRole.Advice.LifetimeAt) record->LivingRole.Advice.LifetimeAt = now;
        // A terminal response still releases its occupied slot. New chat only
        // changes the revision; it cannot create parallel requests per NPC.
        uint64 turnId = ++_npcAgentUtterance;
        if (state.Speech.PinPlayer({turnId, player->GetGUID(), player->GetName(), input.Text, now}))
        {
            state.Conversation = turnId; state.Hop = 0; ++state.Revision;
            state.NextThinkAt = now;
            _needsUpdates.Wake(record->Id, now + 1);
        }
    }
}

void AIWorldMgr::StopNpcAgentAction(AgentRecord& record, Creature& creature, char const* result)
{
    auto& state = _npcAgentStates[record.Id.Value];
    if (creature.GetGUID() == state.Guid && OwnNpcAction(record, state))
    {
        auto type = record.ActiveActionState->Type;
        if (type == ActionType::MoveTo) _actionExecutor.StopMoveTo(creature);
        else if (type == ActionType::Flee) _actionExecutor.FinishRoleFlee(creature, record.LivingRole.TargetGuid);
        else if (type == ActionType::Ambient) _actionExecutor.StopAmbient(creature, record.LivingRole.OwnedStandState);
        record.ActiveActionState.reset(); record.ActiveGoalState.reset();
        record.LivingRole.CurrentPhase = Phase::Idle;
        record.LivingRole.Activity = Activity::None;
        record.LivingRole.OwnedStandState = 0;
        record.LivingRole.TargetGuid.Clear();
        record.LivingRole.MovementPurpose = "NONE";
    }
    state.Active = "NONE"; state.LastResult = result; state.ActionAt = state.ActionUntil = 0;
    state.MealDepot = 0; ++state.Revision; state.NextThinkAt = GetCurrentTimeMs();
}

void AIWorldMgr::NpcAgentActionCompleted(AgentRecord& record, ActionCompletion const& completion)
{
    if (!IsNpcAgent(record)) return;
    auto found = _npcAgentStates.find(record.Id.Value);
    if (found == _npcAgentStates.end() || !OwnNpcAction(record, found->second) ||
        completion.GoalStartedAtMs != found->second.ActionAt || completion.Type != ActionType::MoveTo ||
        completion.SourceGoal != record.ActiveActionState->SourceGoal) return;
    auto& state = found->second;
    state.LastResult = completion.Status == ActionCompletionStatus::Succeeded && completion.Reason == ActionCompletionReason::Arrived ? "ARRIVED" : "MOVE_FAILED";
    state.Active = "NONE"; state.ActionAt = state.ActionUntil = 0; ++state.Revision;
    state.NextThinkAt = completion.CompletedAtMs;
    record.ActiveGoalState.reset(); record.LivingRole.CurrentPhase = Phase::Idle;
    record.LivingRole.MovementPurpose = "NONE";
    _needsUpdates.Wake(record.Id, completion.CompletedAtMs + 1);
}

bool AIWorldMgr::ExecuteNpcAgentAction(AgentRecord& record, Creature& creature,
    NpcAgentActionBinding const& binding, uint64 now)
{
    auto& state = _npcAgentStates[record.Id.Value];
    if (binding.Type != "WAIT")
        state.LastChoice = binding.Type + (binding.Label.empty() ? "" : ": " + binding.Label);
    auto reject = [&](std::string const& reason) { state.LastResult = reason; return false; };
    if (!IsNpcAgent(record) || state.Guid != creature.GetGUID() || !NpcAvailable(creature, GetSimulationScope())) return reject("ACTOR_UNAVAILABLE");
    if (binding.Type == "WAIT") return true;
    if (binding.Type == "STOP") { StopNpcAgentAction(record, creature, "STOPPED_BY_MODEL"); return true; }
    if (state.ActionAt && binding.Type != "FLEE") return reject("OWN_ACTION_BUSY");
    // Check that the offered escape is still feasible before interrupting the
    // model's current activity. The Action API rechecks the same live source.
    Unit* threat = binding.Type == "FLEE" ? NpcThreat(creature) : nullptr;
    if (binding.Type == "FLEE" && !threat) return reject("FLEE_SOURCE_LOST");
    if (binding.Type == "FLEE" && state.Active == "FLEE") return reject("ALREADY_FLEEING");
    if (binding.Type == "FLEE" && state.ActionAt) StopNpcAgentAction(record, creature, "INTERRUPTED_BY_MODEL");
    if (record.ActiveActionState || creature.GetMotionMaster()->GetCurrentMovementGenerator(MOTION_SLOT_ACTIVE)) return reject("ENGINE_ACTION_BUSY");
    ActionRequest request;
    request.Actor = record.Id; request.SourceGoal = GoalType::LocalActivity; request.GoalStartedAtMs = now;
    request.Type = binding.Type == "MOVE_TO" ? ActionType::MoveTo : ActionType::Ambient;
    request.AmbientActivity = binding.Type == "REST" ? Activity::Rest : Activity::Eat;
    ActionValidationContext context;
    context.Materialized = record.WorldState == AgentWorldState::Materialized; context.Alive = creature.IsAlive();
    context.ControlMode = record.ControlMode; context.Scope = GetSimulationScope();
    context.MapId = creature.GetMapId(); context.X = creature.GetPositionX(); context.Y = creature.GetPositionY(); context.Z = creature.GetPositionZ();
    context.InCombat = creature.IsInCombat(); context.ActiveGoalType = request.SourceGoal; context.ActiveGoalStartedAtMs = now;
    context.HasActiveMovement = creature.GetMotionMaster()->GetCurrentMovementGenerator(MOTION_SLOT_ACTIVE) != nullptr;
    context.LivingRoleAllowed = LivingRolePolicy::InScope(_livingRolesEnabled, record.ControlMode,
        creature.GetMapId(), creature.GetZoneId(), _spawnParticipationCatalog.Resolve(record.SpawnId), GetSimulationScope());
    context.LivingRole = ResolveLivingRole(record, creature);
    context.LivingRoleZoneId = creature.GetZoneId(); context.LivingRoleExtensionsAllowed = _livingRoleExtensionsEnabled;
    context.ExpectedAmbientActivity = request.AmbientActivity;
    context.WildlifeRestAllowed = creature.GetStandState() == UNIT_STAND_STATE_STAND;
    if (binding.Type == "FLEE")
    {
        request.Type = ActionType::Flee; request.SourceGoal = GoalType::FleeDanger;
        request.FleeFromGuid = context.FleeSourceGuid = threat->GetGUID(); context.ActiveGoalType = request.SourceGoal;
    }
    else if (binding.Type == "MOVE_TO")
    {
        auto const& home = creature.GetHomePosition();
        ActionPosition homePoint{creature.GetMapId(), home.GetPositionX(), home.GetPositionY(), home.GetPositionZ()};
        NavigationDiagnostics navigation;
        // Resolve a short leg along the complete route to the model's chosen
        // destination. A straight-line midpoint may lie inside an obstacle.
        auto leg = LivingRecoveryPath::Toward(creature, binding.Destination, homePoint, 384.0f, &navigation);
        if (!leg) return reject(navigation.Failure);
        request.Destination = *leg;
        request.Recovery = RecoveryMovement{*leg, homePoint, 384.0f, std::nullopt, false};
        request.Recovery->DangerRadius = LivingRolePolicy::SafetyRadius(record.Id.Value);
        context.ApprovedRecovery = request.Recovery;
    }
    else if (binding.Type == "EAT")
    {
        if (binding.Depot)
        {
            auto location = LivingSupply::DepotLocation(binding.Depot); auto depot = _supplyLedger.GetDepot(binding.Depot);
            auto consumer = SupplyConsumerSpec(record);
            if (!IsSupplyConsumer(record) || !consumer || consumer->Depot != binding.Depot || !location ||
                location->Point.MapId != creature.GetMapId() || !depot || !depot->Stock ||
                creature.GetExactDist(location->Point.X, location->Point.Y, location->Point.Z) > 3 ||
                !creature.IsWithinLOS(location->Point.X, location->Point.Y, location->Point.Z)) return reject("DEPOT_MEAL_UNAVAILABLE");
        }
        else if (!record.EconomyState.Food) return reject("FOOD_UNAVAILABLE");
    }
    else if (binding.Type != "REST") return reject("UNKNOWN_ACTION");
    auto validation = _actionSystem.Validate(request, context);
    if (!validation.Allowed) { state.LastResult = ToString(validation.Reason); return false; }
    ActionResult result = request.Type == ActionType::MoveTo ? _actionExecutor.ExecuteMoveTo(request, creature) :
        request.Type == ActionType::Flee ? _actionExecutor.ExecuteFlee(request, creature, *threat, 0, &GetSimulationScope()) :
        _actionExecutor.ExecuteAmbient(request, creature);
    if (result.Status != ActionExecutionStatus::Started)
        return reject(result.RecoveryNavigation ? result.RecoveryNavigation->Failure : ToString(result.Reason));
    ActiveGoal goal; goal.Type = request.SourceGoal; goal.StartedAtMs = now; record.ActiveGoalState = goal;
    ActiveAction action; action.Type = request.Type; action.SourceGoal = request.SourceGoal;
    action.StartedAtMs = action.GoalStartedAtMs = now; action.Destination = request.Destination; record.ActiveActionState = action;
    auto& role = record.LivingRole;
    role.RuntimeGuid = creature.GetGUID(); role.StartedAtMs = now; role.SourceGoal = request.SourceGoal;
    if (!role.Advice.LifetimeAt) role.Advice.LifetimeAt = now;
    role.CurrentPhase = request.Type == ActionType::MoveTo ? Phase::Moving : request.Type == ActionType::Flee ? Phase::Fleeing : Phase::Acting;
    role.Activity = request.Type == ActionType::Ambient ? request.AmbientActivity : Activity::None;
    role.OwnedStandState = request.Type == ActionType::Ambient && request.AmbientActivity == Activity::Rest ?
        uint8(creature.GetStandState()) : uint8(UNIT_STAND_STATE_STAND);
    role.TargetGuid = threat ? threat->GetGUID() : ObjectGuid::Empty;
    role.MovementPurpose = request.Type == ActionType::MoveTo ? "LLM_MOVE" : "LLM_ACTION";
    if (request.Destination) role.Destination = *request.Destination;
    state.ActionAt = now; state.Active = binding.Type; state.MealDepot = binding.Depot;
    state.ActionUntil = now + (binding.Type == "EAT" ? 5000 : binding.Type == "REST" ? 20000 : binding.Type == "FLEE" ? 10000 : 30000);
    state.LastResult = "STARTED"; ++state.Revision;
    return true;
}

void AIWorldMgr::UpdateNpcAgent(AgentRecord& record, Creature& creature, uint64 now)
{
    auto& state = _npcAgentStates[record.Id.Value];
    if (state.Guid != creature.GetGUID())
    {
        // Registry binding already releases the previous incarnation's proven
        // leases. Initializing this controller must not erase another current
        // engine owner's action without performing its own ownership checks.
        state = {}; state.Guid = creature.GetGUID();
    }
    if (!state.ActionAt && !record.ActiveGoalState && !record.ActiveActionState)
        record.LivingRole.RuntimeGuid = state.Guid;
    if (!record.LivingRole.Advice.LifetimeAt) record.LivingRole.Advice.LifetimeAt = now;
    if (!NpcAvailable(creature, GetSimulationScope()))
    { StopNpcAgentAction(record, creature, "UNAVAILABLE"); state.InvalidatePending(); return; }
    if (!state.AppearanceApplied)
    {
        auto const* appearance = _npcAgentAppearances.Get(record.SpawnId);
        state.AppearanceApplied = !appearance || ApplyNpcAgentAppearance(creature, *appearance);
    }
    ExpireNpcSpeechTurns(state, now);
    if (state.ActionAt)
    {
        bool owned = OwnNpcAction(record, state);
        if (!owned) { state.Active = "NONE"; state.ActionAt = state.ActionUntil = 0; state.MealDepot = 0;
            state.LastResult = "OWNERSHIP_LOST"; ++state.Revision; }
        else if (state.Active == "MOVE_TO" && !OwnPoint(creature))
        {
            bool arrived = record.ActiveActionState->Destination && creature.GetExactDist(
                record.ActiveActionState->Destination->X, record.ActiveActionState->Destination->Y,
                record.ActiveActionState->Destination->Z) <= 2;
            StopNpcAgentAction(record, creature, arrived ? "ARRIVED" : "MOVE_INTERRUPTED");
        }
        else if ((state.Active == "EAT" || state.Active == "REST") &&
            (creature.IsInCombat() || !creature.IsStopped() || (record.LivingRole.OwnedStandState &&
                creature.GetStandState() != record.LivingRole.OwnedStandState)))
            StopNpcAgentAction(record, creature, "ACTIVITY_INTERRUPTED");
        else if (NpcAgentActivityCanComplete(state.Active, state.ActionAt, now, creature.IsAlive(), creature.IsInCombat(),
            creature.IsStopped(), !record.LivingRole.OwnedStandState || creature.GetStandState() == record.LivingRole.OwnedStandState, owned))
        {
            bool completed = state.Active == "REST";
            if (state.Active == "EAT" && state.MealDepot)
            {
                auto found = _supplyActors.find(record.SpawnId); auto location = LivingSupply::DepotLocation(state.MealDepot);
                auto consumer = SupplyConsumerSpec(record);
                if (IsSupplyConsumer(record) && consumer && consumer->Depot == state.MealDepot &&
                    found != _supplyActors.end() && location && location->Point.MapId == creature.GetMapId() &&
                    found->second.Guid == creature.GetGUID() &&
                    found->second.EventSerial < std::numeric_limits<uint64>::max() &&
                    creature.GetExactDist(location->Point.X, location->Point.Y, location->Point.Z) <= 3 &&
                    creature.IsWithinLOS(location->Point.X, location->Point.Y, location->Point.Z))
                {
                    auto meal = _supplyLedger.EatDepot(record.SpawnId, ++found->second.EventSerial, state.MealDepot, 1, true);
                    completed = meal.Success && !meal.Duplicate;
                    if (completed) found->second.Consumed += meal.Portions;
                }
            }
            else if (state.Active == "EAT" && record.EconomyState.Food)
                MutateEconomyAndPersist(record, [&](AgentEconomyState& economy) { completed = LivingRolePolicy::ConsumeStockMeal(economy); });
            if (completed)
            {
                if (state.Active == "EAT") _needsSystem.SatisfyHunger(record.Needs);
                else record.Needs.Fatigue = 0;
            }
            StopNpcAgentAction(record, creature, completed ? "COMPLETED" : "FOOD_UNAVAILABLE");
        }
        else if (now >= state.ActionUntil) StopNpcAgentAction(record, creature, "ACTION_TIME_LIMIT");
    }
    if (state.RequestId || now < state.NextThinkAt || !_aiClient) return;
    auto const* profile = _npcAgentProfiles.Get(record.Id);
    if (!profile) return;
    NpcAgentRequest request; request.Agent = record.Id; request.SnapshotSequence = ++state.Sequence;
    auto& context = request.Context;
    context.Profile = {profile->Name, profile->Backstory, profile->Personality}; context.Needs = record.Needs;
    context.Self = {creature.GetMapId(), creature.GetPositionX(), creature.GetPositionY(), creature.GetPositionZ(), creature.GetOrientation(),
        creature.GetHealth(), creature.GetMaxHealth(), creature.IsAlive(), creature.IsInCombat(), state.Active, state.LastResult};
    context.Self.LastActionResult = NpcAgentActionFeedback(state.LastResult, state.LastChoice);
    NpcAgentDialogueHistory const* history = &state.Dialogue;
    if (state.Speech.PlayerTurn) history = state.Speech.PrivateHistory(state.Speech.PlayerTurn->Partner);
    if (history)
        for (auto const& line : history->Lines()) context.RecentDialogue.push_back({line.SpeakerName, line.Text, line.Kind});
    state.Actions.clear(); state.Recipients.clear();
    auto offer = [&](std::string type, std::string description, ActionPosition point = {}, uint32 depot = 0)
    {
        uint32 token = uint32(context.Actions.size() + 1);
        state.Actions.emplace(token, NpcAgentActionBinding{type, point, depot, description});
        context.Actions.push_back({token, std::move(type), std::move(description)});
    };
    offer("WAIT", "Pokračuj v současné činnosti nebo chvíli počkej.");
    if (state.ActionAt) offer("STOP", "Přeruš vlastní probíhající činnost nebo přesun.");
    if (state.Active != "FLEE" && NpcThreat(creature)) offer("FLEE", "Uteč před svým skutečným současným útočníkem.");
    if (!state.ActionAt && !creature.IsInCombat() && !creature.GetMotionMaster()->GetCurrentMovementGenerator(MOTION_SLOT_ACTIVE))
    {
        if (LivingRolePolicy::Allows(ResolveLivingRole(record, creature), Activity::Rest) &&
            creature.GetStandState() == UNIT_STAND_STATE_STAND)
            offer("REST", "Odpočívej 20 sekund; přerušení odpočinku neobnoví síly.");
        if (record.EconomyState.Food && LivingRolePolicy::Allows(ResolveLivingRole(record, creature), Activity::Eat))
            offer("EAT", "Sněz jednu porci ze svých skutečných zásob (5 sekund).");
        auto addMove = [&](ActionPosition point, std::string label)
        {
            float distance = creature.GetExactDist(point.X, point.Y, point.Z);
            if (distance > 2 && GetSimulationScope().ContainsPosition(point.MapId, point.X, point.Y, point.Z)) offer("MOVE_TO", std::move(label), point);
        };
        if (IsSupplyConsumer(record))
            if (auto spec = SupplyConsumerSpec(record))
                if (auto location = LivingSupply::DepotLocation(spec->Depot))
                {
                    auto depot = _supplyLedger.GetDepot(spec->Depot);
                    if (depot && depot->Stock && creature.GetExactDist(location->Point.X, location->Point.Y, location->Point.Z) <= 3 &&
                        creature.IsWithinLOS(location->Point.X, location->Point.Y, location->Point.Z))
                        offer("EAT", "Sněz jednu porci v místní zásobárně (5 sekund).", {}, spec->Depot);
                    addMove(location->Point, "Jdi směrem k místní zásobárně jídla (zásoby: " + std::to_string(depot ? depot->Stock : 0) + ").");
                }
        auto const home = creature.GetHomePosition(); addMove({creature.GetMapId(), home.GetPositionX(), home.GetPositionY(), home.GetPositionZ()}, "Jdi směrem ke svému domovu.");
        static char const* directions[] = {"východ", "severovýchod", "sever", "severozápad", "západ", "jihozápad", "jih", "jihovýchod"};
        for (unsigned i = 0; i < 8; ++i)
        { float angle = float(i) * float(M_PI) / 4; addMove({creature.GetMapId(), creature.GetPositionX()+12*std::cos(angle), creature.GetPositionY()+12*std::sin(angle), creature.GetPositionZ()}, std::string("Jdi přibližně 12 yardů směrem ") + directions[i] + "."); }
    }
    auto recipient = [&](Unit& unit, bool player)
    {
        if (context.Nearby.size() >= 16 || !VisiblePartner(creature, unit)) return;
        for (auto const& [token, existing] : state.Recipients)
        { (void)token; if (existing.Guid == unit.GetGUID()) return; }
        std::string name = unit.GetName();
        if (!player)
            if (Creature* npc = unit.ToCreature())
                if (AgentRecord const* otherRecord = _registry.FindBySpawn(npc->GetMapId(), npc->GetSpawnId());
                    otherRecord && IsNpcAgent(*otherRecord))
                    if (auto const* otherProfile = _npcAgentProfiles.Get(otherRecord->Id)) name = otherProfile->Name;
        if (!ValidateNpcAgentPlainText(name, 80, 32)) return;
        uint32 token = uint32(context.Nearby.size()+1);
        context.Nearby.push_back({token, std::move(name), player ? "PLAYER" : "NPC", creature.GetExactDist(&unit)});
        context.SpeechTargets.push_back(token); state.Recipients.emplace(token, NpcAgentRecipientBinding{unit.GetGUID(), player});
        // Semantic approach choices let the model answer "come here" without
        // guessing a player's direction. Bind the observed position only;
        // actual movement is still revalidated when the response arrives.
        float distance = creature.GetExactDist(&unit);
        if (!state.ActionAt && !creature.IsInCombat() && distance > 4 && context.Actions.size() < 32)
        {
            float scale = std::min(12.0f, distance - 2.5f) / distance;
            ActionPosition point{creature.GetMapId(),
                creature.GetPositionX() + (unit.GetPositionX()-creature.GetPositionX())*scale,
                creature.GetPositionY() + (unit.GetPositionY()-creature.GetPositionY())*scale,
                creature.GetPositionZ() + (unit.GetPositionZ()-creature.GetPositionZ())*scale};
            if (GetSimulationScope().ContainsPosition(point.MapId, point.X, point.Y, point.Z))
                offer("MOVE_TO", "Přibliž se k postavě " + context.Nearby.back().Name + ".", point);
        }
    };
    if (state.Speech.PlayerTurn)
        if (Player* player = ObjectAccessor::FindConnectedPlayer(state.Speech.PlayerTurn->Partner)) recipient(*player, true);
    if (!state.Speech.PlayerTurn && state.Speech.NpcTurn)
        if (Unit* npc = ObjectAccessor::GetUnit(creature, state.Speech.NpcTurn->Partner)) recipient(*npc, false);
    std::vector<Player*> players; creature.GetPlayerListInGrid(players, 25);
    std::sort(players.begin(), players.end(), [&](Player* a, Player* b) { return creature.GetExactDistSq(a) < creature.GetExactDistSq(b); });
    for (Player* player : players) recipient(*player, true);
    for (auto const& [id, other] : _npcAgentStates)
        if (id != record.Id.Value)
            if (auto* otherRecord = _registry.Find(AgentId{id}); otherRecord && IsNpcAgent(*otherRecord))
                if (auto* npc = creature.GetMap()->GetCreature(other.Guid)) recipient(*npc, false);
    context.Emotes = {"NONE", "WAVE", "TALK", "NOD", "LAUGH", "POINT", "CHEER", "NO"};
    context.Speech.emplace(); // An explicit grant; history alone never authorizes a reply.
    uint64 scene = NpcSpeechScene(state, context);
    NpcAgentPendingTurn const* turn = state.Speech.PlayerTurn ? &*state.Speech.PlayerTurn :
        state.Speech.NpcTurn ? &*state.Speech.NpcTurn : nullptr;
    if (turn)
    {
        uint32 token = 0;
        for (auto const& [candidate, binding] : state.Recipients) if (binding.Guid == turn->Partner) { token = candidate; break; }
        if (token && (state.Speech.PlayerTurn || state.Speech.CanAmbient(now)))
        {
            context.Speech->Mode = state.Speech.PlayerTurn ? "PLAYER_REPLY" : "NPC_REPLY";
            context.Speech->Turn = NpcAgentSpeechTurn{turn->Id, turn->Name, token, turn->Text};
        }
    }
    else if (state.Speech.CanInitiate(scene, !context.Nearby.empty(), now)) context.Speech->Mode = "INITIATE";
    AIRequest transport; transport.NpcAgent = request;
    transport.NpcProvenance = {state.Guid, state.Revision, turn ? turn->Id : 0, turn ? turn->Partner : ObjectGuid::Empty};
    uint64 submitted = _aiClient->SubmitNpcAgent(std::move(transport));
    if (!submitted) { state.Status = "WAITING_MODEL_SLOT"; state.NextThinkAt = now + 1000; return; }
    request.RequestId = submitted; state.Pending = std::move(request); state.RequestId = submitted; state.RequestAt = now;
    if (state.Pending->Context.Speech->Mode == "INITIATE") state.Speech.SubmittedInitiation(scene);
    state.Status = "THINKING"; state.NextThinkAt = now + _npcAgentIntervalMs;
}

bool AIWorldMgr::EmitNpcAgentSpeech(AgentRecord& record, Creature& creature, NpcAgentRuntime& state,
    std::string const& text, std::optional<NpcAgentRecipientBinding> const& recipient, uint64 now)
{
    if (!ValidateNpcAgentSpeechText(text) || !NpcAvailable(creature, GetSimulationScope())) return false;
    ExpireNpcSpeechTurns(state, now);
    bool playerReply = recipient && recipient->Player && state.Speech.PlayerTurn &&
        state.Speech.PlayerTurn->Partner == recipient->Guid;
    bool npcReply = !playerReply && recipient && !recipient->Player && state.Speech.NpcTurn &&
        state.Speech.NpcTurn->Partner == recipient->Guid;
    std::string_view mode = playerReply ? "PLAYER_REPLY" : npcReply ? "NPC_REPLY" : "INITIATE";
    uint64 turnId = playerReply ? state.Speech.PlayerTurn->Id : npcReply ? state.Speech.NpcTurn->Id : 0;
    ObjectGuid partner = playerReply || npcReply ? recipient->Guid : ObjectGuid::Empty;
    if ((recipient && recipient->Player && !playerReply) || !state.Speech.CanSend(mode, turnId, partner, text, now)) return false;
    NpcAgentSpeech speech; speech.Speaker = record.Id; speech.SpeakerName = _npcAgentProfiles.Get(record.Id)->Name;
    speech.Text = text; speech.AtMs = now; speech.Kind = "NPC"; speech.UtteranceId = ++_npcAgentUtterance;
    speech.ConversationId = playerReply ? turnId : npcReply && state.Conversation ? state.Conversation : speech.UtteranceId;
    speech.Hop = playerReply ? 0 : npcReply ? state.Hop + 1 : 1;
    Unit* target = recipient ? ObjectAccessor::GetUnit(creature, recipient->Guid) : nullptr;
    if (recipient && (!target || !VisiblePartner(creature, *target) || (playerReply && !target->ToPlayer()))) return false;
    if (!playerReply && !_npcAgentConversationGuard.Admit(speech, now)) return false;
    if (!SendNpcAgentChat(creature, target, speech.SpeakerName, text, playerReply)) return false;
    if (!state.Speech.CommitSent(mode, turnId, partner, speech, now)) return false;
    if (playerReply) { ++state.Revision; return true; }
    auto ownLine = speech; ownLine.Kind = "SELF"; state.Dialogue.Remember(ownLine);
    for (auto& [id, other] : _npcAgentStates)
    {
        if (id == record.Id.Value || (recipient && other.Guid != recipient->Guid)) continue;
        ExpireNpcSpeechTurns(other, now);
        if (other.Speech.PlayerTurn || !other.Speech.CanAmbient(now)) continue;
        AgentRecord* receiverRecord = _registry.Find(AgentId{id});
        Creature* receiver = receiverRecord && IsNpcAgent(*receiverRecord) ? creature.GetMap()->GetCreature(other.Guid) : nullptr;
        if (!receiver || !NpcAvailable(*receiver, GetSimulationScope()) || !VisiblePartner(*receiver, creature) ||
            !other.Dialogue.Remember(speech) || !other.Speech.PinNpc({speech.UtteranceId, creature.GetGUID(), speech.SpeakerName, text, now}, now)) continue;
        other.Conversation = speech.ConversationId; other.Hop = speech.Hop; ++other.Revision;
        other.NextThinkAt = std::min(other.NextThinkAt, now + 2000);
    }
    return true;
}

void AIWorldMgr::HandleNpcAgentResponse(AIResponse const& response)
{
    auto found = _npcAgentStates.find(response.Agent.Value);
    AgentRecord* record = _registry.Find(response.Agent);
    if (found == _npcAgentStates.end() || !record || !IsNpcAgent(*record)) return;
    auto& state = found->second;
    if (state.RequestId != response.RequestId) return;
    uint64 now = GetCurrentTimeMs();
    ExpireNpcSpeechTurns(state, now);
    Map* map = sMapMgr->FindBaseNonInstanceMap(record->MapId);
    Creature* creature = map ? map->GetCreature(record->RuntimeGuid) : nullptr;
    bool fresh = creature && NpcAvailable(*creature, GetSimulationScope()) &&
        NpcAgentReplyIsFresh(state, response.RequestId, creature->GetGUID(), response.NpcProvenance.RuntimeGuid,
            response.NpcProvenance.ContextRevision, now, _npcAgentTimeoutMs + 2000);
    auto pending = std::move(state.Pending); auto actions = std::move(state.Actions); auto recipients = std::move(state.Recipients);
    state.RequestId = state.RequestAt = 0; state.Pending.reset();
    state.NextThinkAt = now + _npcAgentIntervalMs;
    if (!fresh || !response.Success || !response.NpcAgent || !pending || !MatchesNpcAgentResponse(*pending, *response.NpcAgent))
    {
        state.Status = !fresh ? "STALE_REPLY" : "MODEL_UNAVAILABLE"; state.LastResult = state.Status;
        state.NextThinkAt = now + (!fresh ? 1000 : std::min(60000u, _npcAgentIntervalMs * (1 + std::min(++state.Failures, 4u))));
        return;
    }
    state.Failures = 0; state.Status = "ACTIVE";
    auto const& decision = response.NpcAgent->Decision;
    if (decision.ActionToken)
    {
        auto action = actions.find(decision.ActionToken);
        if (action == actions.end()) state.LastResult = "UNKNOWN_ACTION_TOKEN";
        else ExecuteNpcAgentAction(*record, *creature, action->second, now);
    }
    if (!decision.Speech.empty())
    {
        std::optional<NpcAgentRecipientBinding> target;
        bool permitted = false;
        auto const& grant = pending->Context.Speech;
        if (grant && (grant->Mode == "PLAYER_REPLY" || grant->Mode == "NPC_REPLY") && grant->Turn)
        {
            auto const& turn = grant->Mode == "PLAYER_REPLY" ? state.Speech.PlayerTurn : state.Speech.NpcTurn;
            if (turn && turn->Id == grant->Turn->Id && response.NpcProvenance.DialogueTurnId == turn->Id &&
                response.NpcProvenance.DialoguePartnerGuid == turn->Partner)
            { target = NpcAgentRecipientBinding{turn->Partner, grant->Mode == "PLAYER_REPLY"}; permitted = true; }
        }
        else if (grant && grant->Mode == "INITIATE" && !state.Speech.PlayerTurn && !state.Speech.NpcTurn &&
            !response.NpcProvenance.DialogueTurnId && response.NpcProvenance.DialoguePartnerGuid.IsEmpty())
        {
            permitted = true;
            if (decision.SpeechTarget)
            {
                auto it = recipients.find(decision.SpeechTarget);
                if (it == recipients.end() || it->second.Player) permitted = false;
                else target = it->second;
            }
        }
        if (permitted) EmitNpcAgentSpeech(*record, *creature, state, decision.Speech, target, now);
    }
    if (decision.Emote != "NONE")
    {
        static std::unordered_map<std::string, Emote> const emotes{{"WAVE", EMOTE_ONESHOT_WAVE}, {"TALK", EMOTE_ONESHOT_TALK},
            {"NOD", EMOTE_ONESHOT_YES}, {"LAUGH", EMOTE_ONESHOT_LAUGH}, {"POINT", EMOTE_ONESHOT_POINT},
            {"CHEER", EMOTE_ONESHOT_CHEER}, {"NO", EMOTE_ONESHOT_NO}};
        if (auto it = emotes.find(decision.Emote); it != emotes.end()) creature->HandleEmoteCommand(it->second);
    }
}

std::optional<NpcAgentTelemetry> AIWorldMgr::DescribeNpcAgent(Creature const& creature, AgentRecord const& record) const
{
    auto it = _npcAgentStates.find(record.Id.Value);
    if (!IsNpcAgent(record) || it == _npcAgentStates.end() || it->second.Guid != creature.GetGUID()) return std::nullopt;
    auto const& state = it->second; NpcAgentTelemetry telemetry;
    telemetry.Name = _npcAgentProfiles.Get(record.Id)->Name; telemetry.Status = state.Status;
    telemetry.LastActionResult = state.LastResult; telemetry.ActiveAction = state.Active; telemetry.RequestId = state.RequestId;
    uint64 now = GetCurrentTimeMs(); telemetry.NextThinkMs = state.NextThinkAt > now ? state.NextThinkAt-now : 0;
    telemetry.DialogueCount = uint32(state.Dialogue.Lines().size());
    for (auto line = state.Dialogue.Lines().rbegin(); line != state.Dialogue.Lines().rend(); ++line)
        if (line->Kind == "SELF") { telemetry.LastSpeech = line->Text; break; }
    return telemetry;
}
