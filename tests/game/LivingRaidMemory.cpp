/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/LivingRaidMemory.h"
#include "Memory/ShortTermMemory.h"
#include "Memory/LongTermMemory.h"
#include "Memory/MemoryRetrieval.h"
#include <limits>

namespace
{
    using namespace LivingRaidMemory;
    constexpr AgentId Owner{901055};
    constexpr uint64 Now=10000000;
    constexpr ActionPosition Here{725,0,0,0};
    RetrievedMemory Fact(RaidExperienceKind kind, uint32 depot, ActionPosition point, uint64 age=0,
        uint32 quantity=0, uint32 route=0, uint64 attempt=1, uint64 guard=901026)
    {
        RetrievedMemory memory;
        memory.Owner=Owner; memory.Type=ObservationType::RaidExperience; memory.MemoryId=1;
        memory.LastObservedAtMs=Now-age; memory.FirstObservedAtMs=memory.LastObservedAtMs;
        memory.Importance=.8f; memory.Relevance=.8f;
        memory.Location={point.MapId,point.X,point.Y,point.Z}; memory.Target.SpawnId=guard;
        memory.Raid=RaidExperience{kind,depot,route,quantity,attempt};
        return memory;
    }
    Candidate Route(uint32 depot, uint32 route, ActionPosition point,
        std::vector<ActionPosition> waypoints={})
    { return {depot,route,point,std::move(waypoints)}; }
    Observation Observe(RaidExperienceKind kind, uint32 depot, uint64 at=Now,
        uint32 quantity=0, uint64 attempt=1, uint64 guard=901026)
    {
        Observation observation; observation.Observer=Owner; observation.Type=ObservationType::RaidExperience;
        observation.ObservedAtMs=at; observation.Location={725,10,20,0};
        observation.Raid=RaidExperience{kind,depot,0,quantity,attempt}; observation.Target.SpawnId=guard;
        return observation;
    }
}

TEST_CASE("Raid experience reuses short-term storage and retrieval without persistence", "[AIWorld][LivingRaidMemory]")
{
    ShortTermMemory storage;
    auto guard=Observe(RaidExperienceKind::GuardEncounter,6);
    REQUIRE(storage.Remember(guard,GuardTtlMs,.9f)==ShortTermMemory::RememberResult::Added);
    guard.ObservedAtMs+=1; guard.Location.X=12;
    REQUIRE(storage.Remember(guard,GuardTtlMs,.9f)==ShortTermMemory::RememberResult::Refreshed);
    REQUIRE(storage.GetForAgent(Owner).size()==1);
    auto stock=Observe(RaidExperienceKind::StockObserved,6,Now+1,12);
    REQUIRE(storage.Remember(stock,StockTtlMs,.65f)==ShortTermMemory::RememberResult::Added);
    REQUIRE(storage.GetActiveForAgent({123},Now+1).empty());
    MemoryRetrieval retrieval;
    MemoryQueryContext context{Owner,Now+1,725,0,0,0};
    auto recalled=retrieval.Retrieve(context,storage.GetActiveForAgent(Owner,Now+1),{},128);
    REQUIRE(recalled.size()==2);
    for (auto const& memory : recalled)
    {
        REQUIRE(memory.Owner==Owner); REQUIRE(memory.Raid.has_value());
        REQUIRE(Active(memory,Owner,725,Now+1));
    }
    LongTermMemory persistent;
    REQUIRE_FALSE(persistent.Remember(guard,.99f).has_value());
    REQUIRE(persistent.GetForAgent(Owner).empty());
    LongTermMemoryRecord loaded; loaded.Type=ObservationType::RaidExperience; loaded.Owner=Owner; loaded.PersistentId=10;
    REQUIRE_FALSE(persistent.AddLoaded(loaded));
    REQUIRE(persistent.GetForAgent(Owner).empty());
    REQUIRE(storage.GetActiveForAgent(Owner,Now+1+GuardTtlMs).empty());
}

TEST_CASE("Raid memory deduplication preserves distinct attempts and guard identities", "[AIWorld][LivingRaidMemory]")
{
    ShortTermMemory storage;
    for (uint64 attempt : {1u,2u})
        storage.Remember(Observe(RaidExperienceKind::Delivered,4,Now,2,attempt),OutcomeTtlMs,.8f);
    storage.Remember(Observe(RaidExperienceKind::Delivered,4,Now+1,2,2),OutcomeTtlMs,.8f);
    for (uint64 guard : {901020u,901026u})
        storage.Remember(Observe(RaidExperienceKind::GuardEncounter,4,Now,0,2,guard),GuardTtlMs,.9f);
    auto rows=storage.GetActiveForAgent(Owner,Now+1);
    REQUIRE(rows.size()==4);
    MemoryRetrieval retrieval;
    auto recalled=retrieval.Retrieve({Owner,Now+1,725,0,0,0},rows,{},128);
    REQUIRE(recalled.size()==4);
}

TEST_CASE("Unknown raid stock is explored and fresh observed supplies affect the plan", "[AIWorld][LivingRaidMemory]")
{
    std::vector<Candidate> routes{Route(1,0,{725,300,0,0}),Route(5,0,{725,100,0,0})};
    auto unknown=Choose(Owner,Now,Here,0,routes,{});
    REQUIRE(unknown.DepotId==5); REQUIRE(unknown.Reason==std::string("EXPLORE_UNKNOWN_STOCK"));
    REQUIRE_FALSE(unknown.KnownStock.has_value());
    auto observed=Fact(RaidExperienceKind::StockObserved,1,{725,300,0,0},0,30);
    auto informed=Choose(Owner,Now,Here,0,routes,{observed});
    REQUIRE(informed.DepotId==1); REQUIRE(informed.KnownStock==30u);
    REQUIRE(informed.StockAgeMs==uint64(0)); REQUIRE(informed.Reason==std::string("REMEMBERED_SUPPLY"));
    observed.LastObservedAtMs=Now-StockTtlMs;
    auto expired=Choose(Owner,Now,Here,0,routes,{observed});
    REQUIRE(expired.DepotId==5); REQUIRE_FALSE(expired.KnownStock.has_value());
}

TEST_CASE("Remembered empty stock defers then expires instead of granting remote knowledge", "[AIWorld][LivingRaidMemory]")
{
    std::vector<Candidate> routes{Route(5,0,{725,100,0,0})};
    auto empty=Fact(RaidExperienceKind::SourceEmpty,5,{725,100,0,0});
    auto waiting=Choose(Owner,Now,Here,0,routes,{empty});
    REQUIRE(waiting.DepotId==0); REQUIRE(waiting.Reason==std::string("DEFER_KNOWN_EMPTY"));
    REQUIRE(waiting.RetryAtMs>Now);
    empty.LastObservedAtMs=Now-StockTtlMs;
    REQUIRE(Choose(Owner,Now,Here,0,routes,{empty}).DepotId==5);
}

TEST_CASE("An observed left guard changes the right-bound route rather than inventing right depot facts", "[AIWorld][LivingRaidMemory]")
{
    auto primary=Route(6,0,{725,200,100,0},{{725,0,100,0}});
    auto alternate=Route(6,1,{725,200,100,0},{{725,60,0,0},{725,60,100,0}});
    std::vector<Candidate> routes{primary,alternate};
    REQUIRE(Choose(Owner,Now,Here,0,routes,{}).RouteId==0);
    auto encountered=Fact(RaidExperienceKind::GuardEncounter,6,{725,0,90,0});
    auto learned=Choose(Owner,Now,Here,0,routes,{encountered});
    REQUIRE(learned.DepotId==6); REQUIRE(learned.RouteId==1);
    REQUIRE(learned.Reason==std::string("AVOID_REMEMBERED_DANGER"));
    REQUIRE_FALSE(learned.KnownStock.has_value()); REQUIRE(learned.RecallCount==1);
    REQUIRE_FALSE(learned.Danger.has_value());
    auto inaccessible=Choose(Owner,Now,Here,0,{primary},{encountered});
    REQUIRE(inaccessible.DepotId==0); REQUIRE(inaccessible.Reason==std::string("DEFER_REMEMBERED_DANGER"));
    REQUIRE(inaccessible.Danger.has_value()); REQUIRE(inaccessible.DangerGuardSpawn==901026);
    REQUIRE(inaccessible.Danger->X==0); REQUIRE(inaccessible.Danger->Y==90);
    encountered.LastObservedAtMs=Now-GuardTtlMs;
    REQUIRE(Choose(Owner,Now,Here,0,routes,{encountered}).RouteId==0);
}

TEST_CASE("Guard learning changes target choice and unobserved success cannot bias it", "[AIWorld][LivingRaidMemory]")
{
    std::vector<Candidate> routes{Route(5,0,{725,100,0,0}),Route(4,0,{725,0,110,0})};
    REQUIRE(Choose(Owner,Now,Here,0,routes,{}).DepotId==5);
    auto danger=Fact(RaidExperienceKind::GuardEncounter,5,{725,100,0,0});
    auto learned=Choose(Owner,Now,Here,0,routes,{danger});
    REQUIRE(learned.DepotId==4); REQUIRE(learned.Reason==std::string("AVOID_REMEMBERED_DANGER"));
    auto theft=Fact(RaidExperienceKind::Theft,4,{725,0,110,0},0,2);
    REQUIRE(Choose(Owner,Now,Here,0,routes,{theft}).DepotId==5);
    auto delivered=Fact(RaidExperienceKind::Delivered,4,{725,0,110,0},0,2);
    REQUIRE(Choose(Owner,Now,Here,0,routes,{delivered}).DepotId==4);
    delivered.Raid->AttemptId=0;
    REQUIRE(Choose(Owner,Now,Here,0,routes,{delivered}).DepotId==5);
}

TEST_CASE("Route failure memories outlive the normal raid cooldown and then expire", "[AIWorld][LivingRaidMemory]")
{
    auto blocked=Fact(RaidExperienceKind::RouteBlocked,6,{725,50,0,0},120001,0,1);
    std::vector<Candidate> routes{Route(6,1,{725,100,0,0})};
    REQUIRE(BlockedTtlMs>120000);
    REQUIRE(Choose(Owner,Now,Here,0,routes,{blocked}).DepotId==0);
    blocked.LastObservedAtMs=Now-BlockedTtlMs;
    REQUIRE(Choose(Owner,Now,Here,0,routes,{blocked}).DepotId==6);
}

TEST_CASE("Guard route choice and physical avoidance share the exact aging boundary", "[AIWorld][LivingRaidMemory]")
{
    std::vector<Candidate> routes{Route(5,0,{725,100,0,0})};
    auto guard=Fact(RaidExperienceKind::GuardEncounter,5,{725,100,0,0},GuardHardAvoidMs-1);
    REQUIRE(ShouldAvoidGuard(guard,Owner,725,Now));
    REQUIRE(Choose(Owner,Now,Here,0,routes,{guard}).DepotId==0);
    guard.LastObservedAtMs=Now-GuardHardAvoidMs;
    REQUIRE(ShouldAvoidGuard(guard,Owner,725,Now));
    REQUIRE(Choose(Owner,Now,Here,0,routes,{guard}).DepotId==0);
    guard.LastObservedAtMs=Now-GuardHardAvoidMs-1;
    REQUIRE_FALSE(ShouldAvoidGuard(guard,Owner,725,Now));
    REQUIRE(Choose(Owner,Now,Here,0,routes,{guard}).DepotId==5);
    guard.LastObservedAtMs=Now-GuardTtlMs;
    REQUIRE_FALSE(Active(guard,Owner,725,Now));
}

TEST_CASE("Raid planning rejects foreign future malformed and persistent evidence", "[AIWorld][LivingRaidMemory]")
{
    std::vector<Candidate> routes{Route(5,0,{725,100,0,0})};
    auto real=Fact(RaidExperienceKind::GuardEncounter,5,{725,100,0,0});
    for (unsigned change=0;change<10;++change)
    {
        auto invalid=real;
        switch (change)
        {
            case 0:invalid.Owner={123};break;
            case 1:invalid.Location.MapId=1;break;
            case 2:invalid.LastObservedAtMs=Now+1;break;
            case 3:invalid.Location.X=std::numeric_limits<float>::quiet_NaN();break;
            case 4:invalid.Tier=MemoryTier::LongTerm;break;
            case 5:invalid.Raid.reset();break;
            case 6:invalid.Raid->DepotId=99;break;
            case 7:invalid.Relevance=std::numeric_limits<float>::infinity();break;
            case 8:invalid.Target.SpawnId=0;break;
            case 9:invalid.Raid->Kind=RaidExperienceKind::StockObserved;invalid.Raid->Quantity=31;break;
        }
        auto result=Choose(Owner,Now,Here,0,routes,{invalid});
        REQUIRE(result.DepotId==5); REQUIRE(result.RecallCount==0);
    }
    REQUIRE(Choose({},Now,Here,0,routes,{real}).DepotId==0);
    REQUIRE(Choose(Owner,Now,{725,std::numeric_limits<float>::infinity(),0,0},0,routes,{}).DepotId==0);
    REQUIRE(Choose(Owner,Now,Here,0,{Route(5,0,{1,0,0,0})},{}).DepotId==0);
}

TEST_CASE("Learned danger checks the whole movement chord and permits only monotone retreat inside", "[AIWorld][LivingRaidMemory]")
{
    ActionPosition danger{725,0,0,0};
    REQUIRE_FALSE(SafeKnownDangerLeg({725,-50,0,0},{725,50,0,0},danger,40));
    REQUIRE(SafeKnownDangerLeg({725,-50,50,0},{725,50,50,0},danger,40));
    REQUIRE(SafeKnownDangerLeg({725,10,0,0},{725,50,0,0},danger,40));
    REQUIRE_FALSE(SafeKnownDangerLeg({725,10,0,0},{725,-50,0,0},danger,40));
    REQUIRE_FALSE(SafeKnownDangerLeg({725,10,0,0},{725,5,0,0},danger,40));
    REQUIRE_FALSE(SafeKnownDangerLeg({725,10,0,0},{1,50,0,0},danger,40));
    REQUIRE_FALSE(SafeKnownDangerLeg({0,10,0,0},{0,50,0,0},{0,0,0,0},40));
    REQUIRE_FALSE(SafeKnownDangerLeg({725,10,0,0},{725,50,0,0},danger,std::numeric_limits<float>::quiet_NaN()));
}
