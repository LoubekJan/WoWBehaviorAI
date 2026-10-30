/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "AIWorldMgr.h"
#include "Creature.h"
#include "Map.h"
#include "MapManager.h"
#include "Agent/LivingRaidMemory.h"
#include <algorithm>
#include <limits>
#include <string_view>

namespace
{
    bool BoundMember(AgentRecord const& record, Creature const* creature, ObjectGuid guid)
    {
        return creature && creature->GetMapId() == 725 && creature->GetSpawnId() == record.SpawnId &&
            record.ControlMode == AgentControlMode::AIWorldControlled && record.WorldState == AgentWorldState::Materialized &&
            record.RuntimeGuid == guid && creature->GetGUID() == guid && creature->IsInWorld() &&
            !creature->IsPet() && !creature->IsCharmed() && creature->GetCharmerOrOwnerGUID().IsEmpty() &&
            !creature->IsControlledByPlayer();
    }
    bool AtAnchor(Creature const& creature, ActionPosition const& target)
    {
        return creature.IsAlive() && creature.IsStopped() && creature.GetMapId() == target.MapId &&
            creature.GetExactDist(target.X, target.Y, target.Z) <= LivingRaid::ArrivalRadius(target) &&
            creature.IsWithinLOS(target.X, target.Y, target.Z);
    }
}

std::vector<AgentId> AIWorldMgr::LivingRaidPartyRoster(AgentRecord const& record, Creature const& creature, uint64 nowMs) const
{
    if (!_livingDefiasEnabled || !IsSupplyRaider(record)) return {};
    if (std::count_if(_raidParties.begin(), _raidParties.end(), [](auto const& item)
        { return item.second.Coordination.Stage != LivingRaidParty::Phase::Complete; }) >= 2) return {};
    std::vector<LivingRaidParty::Candidate> candidates;
    for (uint64 spawn = LivingRaid::FirstRaider; spawn <= LivingRaid::LastRaider; ++spawn)
    {
        auto member = _registry.FindBySpawn(725, spawn);
        auto runtime = _raidRaiders.find(spawn);
        auto actor = _supplyActors.find(spawn);
        Creature* npc = creature.GetMap()->GetCreatureBySpawnId(spawn);
        if (!member || !npc || runtime == _raidRaiders.end() || actor == _supplyActors.end() || !IsSupplyRaider(*member)) continue;
        auto const& raid = runtime->second;
        // An old complete party is diagnostic history, never authority over a
        // new sortie. An in-progress party cannot recruit the same actor twice.
        auto old = _raidParties.find(raid.PartyId);
        bool unassigned = old == _raidParties.end() || old->second.Coordination.Stage == LivingRaidParty::Phase::Complete;
        bool bound = BoundMember(*member, npc, actor->second.Guid) && actor->second.Live &&
            member->LivingRole.RuntimeGuid == npc->GetGUID();
        bool camp = npc->GetExactDist(LivingRaid::Camp.X, LivingRaid::Camp.Y, LivingRaid::Camp.Z) <= 55.0f &&
            npc->IsWithinLOS(LivingRaid::Camp.X, LivingRaid::Camp.Y, LivingRaid::Camp.Z) &&
            creature.IsWithinLOSInMap(npc) && creature.CanSeeOrDetect(npc);
        bool ready = unassigned && !raid.Active && !raid.Returning && !_supplyLedger.GetCargo(spawn) &&
            nowMs >= raid.NextAtMs && member->Needs.Hunger >= .65f && !npc->IsInCombat() && !npc->IsInEvadeMode();
        candidates.push_back({{member->Id, 725, npc->GetPositionX(), npc->GetPositionY(), npc->GetPositionZ(),
            npc->GetEntry(), member->WorldFaction}, spawn,
            member->WorldState == AgentWorldState::Materialized, npc->IsAlive(), bound, camp,
            member->LivingRole.CurrentPhase == LivingRoleState::Phase::Idle && !member->ActiveActionState &&
                !member->ActiveGoalState, ready});
    }
    for (auto const& roster : LivingRaidParty::Form(candidates))
        if (!roster.empty() && roster.front() == record.Id)
        {
            // Let nearby late arrivals join before launching a tiny party;
            // a reduced surviving population may still depart after 20s.
            if (roster.size() < LivingRaidParty::MaxMembers &&
                !LivingRaidParty::Elapsed(nowMs, _raidRaiders.at(record.SpawnId).CampReadyAtMs, 20000)) return {};
            return roster;
        }
    return {};
}

bool AIWorldMgr::CreateLivingRaidParty(AgentRecord& leader, Creature& creature, uint64 nowMs, std::vector<AgentId> const& roster)
{
    if (!_livingDefiasEnabled || roster.size() < LivingRaidParty::MinMembers || roster.size() > LivingRaidParty::MaxMembers ||
        roster.front() != leader.Id || _nextRaidPartyId == std::numeric_limits<uint64>::max()) return false;
    auto const selected = _raidRaiders.at(leader.SpawnId);
    auto source = LivingRaid::SourceFor(selected.SelectedSourceDepot, true, selected.SelectedRouteId);
    if (!source) return false;
    LivingRaid::PartyRuntime runtime;
    auto& party = runtime.Coordination;
    party.Id = party.AttemptId = ++_nextRaidPartyId;
    party.LeaderSpawn = leader.SpawnId;
    party.Members = roster;
    party.SourceDepot = source->Depot; party.RouteId = source->RouteId;
    party.StartedAtMs = party.PhaseStartedAtMs = party.LastProgressAtMs = nowMs;
    party.Reason = "ASSEMBLING";
    // Validate every original incarnation before mutating any member. Party
    // formation never revives an actor or replaces a foreign action owner.
    for (AgentId id : roster)
    {
        auto member = _registry.Find(id);
        auto npc = creature.GetMap()->GetCreatureBySpawnId(id.Value);
        if (!member || !IsSupplyRaider(*member) || !npc || !npc->IsAlive() || npc->IsInCombat() ||
            !BoundMember(*member, npc, member->LivingRole.RuntimeGuid) ||
            member->LivingRole.CurrentPhase != LivingRoleState::Phase::Idle || member->ActiveActionState || member->ActiveGoalState) return false;
        runtime.Lives.emplace(id.Value, npc->GetGUID());
    }
    _raidParties.emplace(party.Id, runtime);
    for (AgentId id : roster)
    {
        auto& raid = _raidRaiders.at(id.Value);
        auto& actor = _supplyActors.at(id.Value);
        raid.PartyId = party.Id; raid.AttemptId = party.AttemptId;
        raid.SelectedSourceDepot = raid.PreviousSourceDepot = party.SourceDepot;
        raid.SelectedRouteId = party.RouteId;
        // A shared order is not a copied memory: every actor retains its own
        // observations and the planner still vetoes its own remembered dangers.
        raid.PlanningReason = selected.PlanningReason; raid.MemoryRisk = selected.MemoryRisk;
        raid.Active = true; raid.Returning = false; raid.Leg = 0;
        raid.StartedAtMs = nowMs; raid.StolenAtStart = actor.Stolen;
        raid.CombatNoted = false;
        raid.CampReadyAtMs = 0;
        raid.Phase = actor.Phase = "ASSEMBLING"; raid.Outcome = "STARTED";
        actor.Travel = {}; actor.NextAtMs = nowMs;
        _registry.Find(id)->LivingRole.NextDecisionAtMs = nowMs;
    }
    // Keep only referenced completed histories, so the session has a bounded
    // number of party records even after many hours of repeated sorties.
    std::erase_if(_raidParties, [&](auto const& item)
    {
        if (item.second.Coordination.Stage != LivingRaidParty::Phase::Complete) return false;
        return std::none_of(_raidRaiders.begin(), _raidRaiders.end(), [&](auto const& actor)
            { return actor.second.PartyId == item.first; });
    });
    return true;
}

ActionPosition AIWorldMgr::LivingRaidPartyDestination(AgentRecord const& record, ActionPosition const& anchor, bool spread) const
{
    auto raid = _raidRaiders.find(record.SpawnId);
    if (!spread || raid == _raidRaiders.end()) return anchor;
    auto group = _raidParties.find(raid->second.PartyId);
    if (group == _raidParties.end()) return anchor;
    // Bridge decks and their ramps stay single-file on the proved centerline.
    // Every other offset is still independently checked by the live path planner.
    float radius = anchor.X < -150.0f && anchor.X > -230.0f && anchor.Y > 830.0f ? 0.0f : 3.0f;
    return LivingRaidParty::FormationTarget(record.Id, group->second.Coordination.Members, anchor, radius).value_or(anchor);
}

void AIWorldMgr::RetreatLivingRaidParty(uint64 partyId, uint64 nowMs, char const* reason)
{
    auto found = _raidParties.find(partyId);
    if (found == _raidParties.end()) return;
    auto& party = found->second.Coordination;
    Map* partyMap = sMapMgr->FindMap(725, 0);
    bool anyLiving = std::any_of(party.Members.begin(), party.Members.end(), [&](AgentId id)
    {
        auto member = _registry.Find(id);
        auto npc = partyMap ? partyMap->GetCreatureBySpawnId(id.Value) : nullptr;
        auto life = found->second.Lives.find(id.Value);
        return member && _raidRaiders.at(id.Value).PartyId == partyId && life != found->second.Lives.end() &&
            BoundMember(*member, npc, life->second) && npc->IsAlive();
    });
    if (!anyLiving)
    { party.Stage = LivingRaidParty::Phase::Complete; party.Reason = "ALL_MEMBERS_UNAVAILABLE"; return; }
    if (party.Stage == LivingRaidParty::Phase::Returning || party.Stage == LivingRaidParty::Phase::Complete) return;
    auto source = LivingRaid::SourceFor(party.SourceDepot, true, party.RouteId);
    party.Leg = source ? std::min(party.Leg, source->Count-1) : 0;
    party.Stage = LivingRaidParty::Phase::Returning;
    party.PhaseStartedAtMs = party.LastProgressAtMs = nowMs;
    party.Reason = reason;
    for (AgentId id : party.Members)
    {
        auto state = _raidRaiders.find(id.Value);
        if (state == _raidRaiders.end() || state->second.PartyId != partyId) continue;
        auto& raid = state->second;
        auto member = _registry.Find(id);
        Map* map = sMapMgr->FindMap(725, 0);
        auto npc = map ? map->GetCreatureBySpawnId(id.Value) : nullptr;
        auto life = found->second.Lives.find(id.Value);
        if (member && life != found->second.Lives.end() && BoundMember(*member, npc, life->second))
        {
            auto const& role = member->LivingRole;
            bool owned = role.SourceGoal == GoalType::LocalActivity && member->ActiveGoalState &&
                member->ActiveGoalState->Type == role.SourceGoal && member->ActiveGoalState->StartedAtMs == role.StartedAtMs &&
                member->ActiveActionState && member->ActiveActionState->SourceGoal == role.SourceGoal &&
                member->ActiveActionState->GoalStartedAtMs == role.StartedAtMs &&
                member->ActiveActionState->StartedAtMs == role.StartedAtMs;
            std::string_view purpose(role.MovementPurpose);
            bool raidMove = purpose == "RAID_SOURCE" || purpose == "RAID_ASSEMBLE";
            if (owned && ((member->ActiveActionState->Type == ActionType::MoveTo && raidMove) ||
                (member->ActiveActionState->Type == ActionType::Ambient && raid.Phase == "STEALING")))
                StopLivingRole(*member, *npc);
            member->LivingRole.NextDecisionAtMs = nowMs;
        }
        _supplyActors.at(id.Value).Travel.RejectPending();
        raid.Active = false; raid.Returning = true; raid.Leg = party.Leg;
        raid.Outcome = reason;
        _supplyActors.at(id.Value).NextAtMs = nowMs;
    }
}

void AIWorldMgr::UpdateLivingRaidParty(AgentRecord& record, Creature& creature, uint64 nowMs)
{
    if (!_livingDefiasEnabled || !IsSupplyRaider(record)) return;
    auto found = _raidParties.find(_raidRaiders.at(record.SpawnId).PartyId);
    if (found == _raidParties.end()) return;
    auto& runtime = found->second;
    auto& party = runtime.Coordination;
    if (party.Stage == LivingRaidParty::Phase::Complete) return;
    auto source = LivingRaid::SourceFor(party.SourceDepot, true, party.RouteId);
    if (!source) { RetreatLivingRaidParty(party.Id, nowMs, "INVALID_SOURCE"); return; }
    std::vector<LivingRaidParty::MemberObservation> observations;
    for (AgentId id : party.Members)
    {
        auto member = _registry.Find(id);
        auto npc = creature.GetMap()->GetCreatureBySpawnId(id.Value);
        auto const life = runtime.Lives.find(id.Value);
        auto& raid = _raidRaiders.at(id.Value);
        bool bound = member && life != runtime.Lives.end() && BoundMember(*member, npc, life->second) && raid.PartyId == party.Id;
        bool living = bound && npc->IsAlive();
        ActionPosition anchor = party.Stage == LivingRaidParty::Phase::Assembling ? LivingRaid::Camp :
            party.Stage == LivingRaidParty::Phase::Returning ? party.Leg ? source->Route[std::min(party.Leg, source->Count)-1] : LivingRaid::Camp :
            source->Route[std::min(party.Leg, source->Count-1)];
        bool spread = party.Stage == LivingRaidParty::Phase::Assembling ||
            (party.Stage == LivingRaidParty::Phase::Outbound && party.Leg+1 < source->Count) ||
            (party.Stage == LivingRaidParty::Phase::Returning && party.Leg);
        if (member) anchor = LivingRaidPartyDestination(*member, anchor, spread);
        bool complete = living && !raid.Active && !raid.Returning && !_supplyLedger.GetCargo(id.Value) &&
            AtAnchor(*npc, LivingRaid::Camp);
        bool arrived = living && AtAnchor(*npc, anchor);
        if (party.Stage == LivingRaidParty::Phase::Raiding) arrived = living && raid.Returning;
        observations.push_back({id, member && member->WorldState == AgentWorldState::Materialized,
            bound && npc->IsAlive(), bound, living && npc->IsInCombat(), arrived, complete});
    }
    auto assessment = LivingRaidParty::Assess(party, observations, nowMs);
    if (assessment.Action == LivingRaidParty::Decision::Retreat)
    { RetreatLivingRaidParty(party.Id, nowMs, assessment.Reason); return; }
    if (assessment.Action == LivingRaidParty::Decision::Complete)
    {
        party.Stage = LivingRaidParty::Phase::Complete;
        party.Reason = assessment.Reason;
        // Outstanding cargo and return routes remain owned by each actor.
        return;
    }
    if (assessment.Action != LivingRaidParty::Decision::Advance)
    { runtime.RallyReadyAtMs = 0; return; }
    if (party.Stage == LivingRaidParty::Phase::Assembling)
    {
        // Give the gathered party a short real rally, rather than turning the
        // last arrival into an unobservable instantaneous phase transition.
        if (!runtime.RallyReadyAtMs) runtime.RallyReadyAtMs = nowMs;
        if (!LivingRaidParty::Elapsed(nowMs, runtime.RallyReadyAtMs, 2000)) return;
        party.Stage = LivingRaidParty::Phase::Outbound;
    }
    else if (party.Stage == LivingRaidParty::Phase::Outbound)
    {
        if (party.Leg+1 < source->Count) ++party.Leg;
        else party.Stage = LivingRaidParty::Phase::Raiding;
    }
    else if (party.Stage == LivingRaidParty::Phase::Raiding)
    { RetreatLivingRaidParty(party.Id, nowMs, "RAID_ACTIONS_COMPLETE"); return; }
    else if (party.Stage == LivingRaidParty::Phase::Returning && party.Leg) --party.Leg;
    else return;
    party.PhaseStartedAtMs = party.LastProgressAtMs = nowMs;
    party.Reason = assessment.Reason;
    for (AgentId id : party.Members)
    {
        auto& raid = _raidRaiders.at(id.Value);
        if (raid.PartyId != party.Id) continue;
        raid.Leg = party.Leg;
        _supplyActors.at(id.Value).Travel = {};
        _supplyActors.at(id.Value).NextAtMs = nowMs;
        if (auto member = _registry.Find(id)) member->LivingRole.NextDecisionAtMs = nowMs;
    }
}
