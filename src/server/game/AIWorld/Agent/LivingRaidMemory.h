/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGRAIDMEMORY_H
#define AIWORLD_LIVINGRAIDMEMORY_H

#include "Action/ActionPosition.h"
#include "Agent/LivingReturnPolicy.h"
#include "Memory/RetrievedMemory.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

// Planning over the agent's existing retrieved short-term memories. None of
// these functions reads world stock, moves a creature, commits food or persists.
namespace LivingRaidMemory
{
    constexpr uint64 GuardTtlMs = 900000, StockTtlMs = 300000;
    constexpr uint64 OutcomeTtlMs = 900000, BlockedTtlMs = 300000;
    constexpr float DangerRadius = 40.0f;
    constexpr float StrongRiskThreshold = 4.0f;
    constexpr uint64 GuardHardAvoidMs = GuardTtlMs*4/5;

    inline uint64 ExperienceTtlMs(RaidExperienceKind kind)
    {
        switch (kind)
        {
            case RaidExperienceKind::StockObserved:
            case RaidExperienceKind::SourceEmpty: return StockTtlMs;
            case RaidExperienceKind::GuardEncounter: return GuardTtlMs;
            case RaidExperienceKind::RouteBlocked: return BlockedTtlMs;
            case RaidExperienceKind::Theft:
            case RaidExperienceKind::Delivered: return OutcomeTtlMs;
        }
        return 0;
    }
    inline float Importance(RaidExperienceKind kind)
    { return kind == RaidExperienceKind::GuardEncounter ? .9f : kind == RaidExperienceKind::Delivered ? .8f : .65f; }

    struct Candidate
    {
        uint32 DepotId = 0, RouteId = 0;
        ActionPosition Point;
        std::vector<ActionPosition> Waypoints;
    };
    struct Decision
    {
        uint32 DepotId = 0, RouteId = 0, RecallCount = 0;
        char const* Reason = "NO_CANDIDATE";
        float Risk = 0, Score = 0;
        std::optional<uint32> KnownStock;
        std::optional<uint64> StockAgeMs;
        std::optional<ActionPosition> Danger;
        float DangerRadius = LivingRaidMemory::DangerRadius;
        uint64 DangerGuardSpawn = 0, DangerAgeMs = 0, RetryAtMs = 0;
    };

    inline bool VillageDepot(uint32 id) { return id == 1 || (id >= 4 && id <= 6); }
    inline bool Active(RetrievedMemory const& memory, AgentId owner, uint32 map, uint64 now)
    {
        if (!owner || memory.Owner != owner || memory.Tier != MemoryTier::ShortTerm ||
            memory.Type != ObservationType::RaidExperience || !memory.Raid ||
            !VillageDepot(memory.Raid->DepotId) || memory.Location.MapId != map ||
            !LivingReturnPolicy::Finite({map, memory.Location.X, memory.Location.Y, memory.Location.Z}) ||
            !memory.LastObservedAtMs || memory.LastObservedAtMs > now ||
            !std::isfinite(memory.Importance) || memory.Importance<0 || memory.Importance>1 ||
            !std::isfinite(memory.Relevance) || memory.Relevance<0 || memory.Relevance>1) return false;
        auto const& fact=*memory.Raid;
        if ((fact.Kind==RaidExperienceKind::GuardEncounter && !memory.Target.SpawnId) ||
            (fact.Kind==RaidExperienceKind::StockObserved && fact.Quantity>30) ||
            ((fact.Kind==RaidExperienceKind::Theft || fact.Kind==RaidExperienceKind::Delivered) &&
                (!fact.Quantity || fact.Quantity>2 || !fact.AttemptId))) return false;
        uint64 ttl = ExperienceTtlMs(memory.Raid->Kind);
        return ttl && now-memory.LastObservedAtMs < ttl;
    }
    inline float Freshness(RetrievedMemory const& memory, uint64 now)
    {
        uint64 ttl = ExperienceTtlMs(memory.Raid->Kind);
        return ttl && now >= memory.LastObservedAtMs ?
            float(std::clamp(1.0-double(now-memory.LastObservedAtMs)/double(ttl),0.0,1.0)) : 0;
    }
    inline bool ShouldAvoidGuard(RetrievedMemory const& memory, AgentId owner, uint32 map, uint64 now)
    {
        return Active(memory,owner,map,now) && memory.Raid->Kind==RaidExperienceKind::GuardEncounter &&
            now-memory.LastObservedAtMs<=GuardHardAvoidMs;
    }
    inline float SegmentDistance(ActionPosition const& point, ActionPosition const& from, ActionPosition const& to)
    {
        double x=double(to.X)-from.X, y=double(to.Y)-from.Y, z=double(to.Z)-from.Z;
        double squared=x*x+y*y+z*z;
        double t=squared ? std::clamp(((double(point.X)-from.X)*x+(double(point.Y)-from.Y)*y+
            (double(point.Z)-from.Z)*z)/squared, 0.0, 1.0) : 0;
        return float(std::hypot(double(point.X)-from.X-t*x, double(point.Y)-from.Y-t*y,
            double(point.Z)-from.Z-t*z));
    }
    inline float RouteDistance(Candidate const& candidate, ActionPosition const& here, ActionPosition const& point)
    {
        ActionPosition previous=here;
        float closest=std::numeric_limits<float>::max();
        for (auto const& next : candidate.Waypoints)
        { closest=std::min(closest, SegmentDistance(point,previous,next)); previous=next; }
        return std::min(closest,SegmentDistance(point,previous,candidate.Point));
    }
    inline bool SafeKnownDangerLeg(ActionPosition const& from, ActionPosition const& to,
        ActionPosition const& danger, float radius)
    {
        using namespace LivingReturnPolicy;
        if (!from.MapId || !Finite(from) || !Finite(to) || !Finite(danger) || from.MapId!=to.MapId ||
            from.MapId!=danger.MapId || !std::isfinite(radius) || radius<=0) return false;
        float source=Distance(from,danger), end=Distance(to,danger);
        float closest=SegmentDistance(danger,from,to);
        if (source>=radius) return closest>=radius;
        // A newly learned threat can contain the actor. Permit a real retreat
        // that never comes closer, while rejecting chords through the threat.
        return end>source && closest+0.0001f>=source;
    }
    inline float RouteLength(Candidate const& candidate, ActionPosition const& here)
    {
        ActionPosition previous=here; float length=0;
        for (auto const& next : candidate.Waypoints)
        { length+=LivingReturnPolicy::Distance(previous,next); previous=next; }
        return length+LivingReturnPolicy::Distance(previous,candidate.Point);
    }

    inline Decision Choose(AgentId owner, uint64 now, ActionPosition const& here, uint32 previousDepot,
        std::vector<Candidate> const& candidates, std::vector<RetrievedMemory> const& memories)
    {
        Decision result;
        if (!owner || !now || !here.MapId || !LivingReturnPolicy::Finite(here) ||
            candidates.size()>16 || memories.size()>128)
        { result.Reason="INVALID_CONTEXT"; return result; }
        std::vector<RetrievedMemory const*> recalled;
        for (auto const& memory : memories)
            if (Active(memory,owner,here.MapId,now)) recalled.push_back(&memory);
        result.RecallCount=uint32(recalled.size());
        bool risky=false, empty=false;
        float best=-std::numeric_limits<float>::max(), strongestRisk=0;
        // Candidates come from bounded server-authored route definitions.
        for (auto const& candidate : candidates)
        {
            auto validPoint=[&](auto const& point)
            { return point.MapId==here.MapId && LivingReturnPolicy::Finite(point); };
            if (!VillageDepot(candidate.DepotId) || !validPoint(candidate.Point) || candidate.Waypoints.size()>16 ||
                !std::all_of(candidate.Waypoints.begin(),candidate.Waypoints.end(),validPoint)) continue;
            RetrievedMemory const* stock=nullptr;
            float reward=3.0f, success=0, risk=0;
            bool hardAvoid=false;
            RetrievedMemory const* danger=nullptr;
            for (auto memory : recalled)
            {
                auto const& fact=*memory->Raid;
                float freshness=Freshness(*memory,now);
                if (fact.DepotId==candidate.DepotId)
                {
                    if ((fact.Kind==RaidExperienceKind::StockObserved || fact.Kind==RaidExperienceKind::SourceEmpty) &&
                        (!stock || memory->LastObservedAtMs>stock->LastObservedAtMs ||
                            (memory->LastObservedAtMs==stock->LastObservedAtMs && memory->MemoryId>stock->MemoryId))) stock=memory;
                    // Theft alone is not a successful supply expedition.
                    if (fact.Kind==RaidExperienceKind::Delivered && fact.Quantity && fact.AttemptId)
                        success=std::max(success,1.5f*freshness*std::clamp(memory->Relevance,0.0f,1.0f));
                    if (fact.Kind==RaidExperienceKind::RouteBlocked && fact.RouteId==candidate.RouteId)
                    {
                        risk=std::max(risk,12.0f*freshness);
                        hardAvoid=hardAvoid || risk>=StrongRiskThreshold;
                    }
                }
                if (fact.Kind==RaidExperienceKind::GuardEncounter)
                {
                    ActionPosition point{here.MapId,memory->Location.X,memory->Location.Y,memory->Location.Z};
                    if (RouteDistance(candidate,here,point)<=DangerRadius)
                    {
                        float dangerRisk=20.0f*freshness;
                        if (!danger || dangerRisk>20.0f*Freshness(*danger,now)) danger=memory;
                        risk=std::max(risk,dangerRisk);
                        hardAvoid=hardAvoid || ShouldAvoidGuard(*memory,owner,here.MapId,now);
                    }
                }
            }
            if (stock)
            {
                uint32 quantity=stock->Raid->Kind==RaidExperienceKind::SourceEmpty ? 0 : stock->Raid->Quantity;
                if (quantity<6) { empty=true; continue; }
                reward=3.5f+float(std::min(quantity,30u))/30.0f*Freshness(*stock,now);
            }
            if (hardAvoid)
            {
                risky=true;
                if (risk>strongestRisk)
                {
                    strongestRisk=risk;
                    if (!result.DepotId && danger)
                    {
                        result.Danger=ActionPosition{here.MapId,danger->Location.X,danger->Location.Y,danger->Location.Z};
                        result.DangerGuardSpawn=danger->Target.SpawnId; result.DangerAgeMs=now-danger->LastObservedAtMs;
                    }
                }
                continue;
            }
            float score=reward+success-RouteLength(candidate,here)/400.0f-risk-(candidate.DepotId==previousDepot ? .35f : 0);
            if (score>best || (score==best && std::pair(candidate.DepotId,candidate.RouteId)<std::pair(result.DepotId,result.RouteId)))
            {
                best=score; result.DepotId=candidate.DepotId; result.RouteId=candidate.RouteId;
                result.Score=score; result.Risk=risk;
                result.KnownStock=stock ? std::optional<uint32>(stock->Raid->Kind==RaidExperienceKind::SourceEmpty ? 0 : stock->Raid->Quantity) : std::nullopt;
                result.StockAgeMs=stock ? std::optional<uint64>(now-stock->LastObservedAtMs) : std::nullopt;
                result.Reason=stock ? "REMEMBERED_SUPPLY" : "EXPLORE_UNKNOWN_STOCK";
                result.Danger=std::nullopt; result.DangerGuardSpawn=result.DangerAgeMs=0;
                if (danger)
                {
                    result.Danger=ActionPosition{here.MapId,danger->Location.X,danger->Location.Y,danger->Location.Z};
                    result.DangerGuardSpawn=danger->Target.SpawnId; result.DangerAgeMs=now-danger->LastObservedAtMs;
                }
            }
        }
        if (!result.DepotId)
        {
            result.Risk=strongestRisk;
            result.Reason=risky ? "DEFER_REMEMBERED_DANGER" : empty ? "DEFER_KNOWN_EMPTY" : "NO_CANDIDATE";
            result.RetryAtMs=now>std::numeric_limits<uint64>::max()-30000 ? std::numeric_limits<uint64>::max() : now+30000;
        }
        else if (risky)
            result.Reason="AVOID_REMEMBERED_DANGER";
        return result;
    }
}
#endif
