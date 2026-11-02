/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#include "tc_catch2.h"
#include "Agent/LivingFineRoute.h"
#include "Agent/LivingSupplyRuntime.h"
#include "Agent/LivingRolePolicy.h"
#include <limits>

namespace
{
    ActionPosition P(float x,float y,float z=0) { return {725,x,y,z}; }
    auto flat = [](ActionPosition const&) -> std::optional<float> { return 0.0f; };
    auto clear = [](ActionPosition const&,ActionPosition const&) { return true; };
    auto inside = [](ActionPosition const&) { return true; };
    auto tile = [](ActionPosition const&) { return true; };
    std::vector<ActionPosition> Corner()
    { return {P(0,0),P(.625f,0),P(1.25f,0),P(1.25f,.625f),P(1.25f,1.25f)}; }
    RecoveryMovement Request(LivingSupply::TravelContext& travel,std::vector<ActionPosition> const& controls)
    {
        RecoveryMovement request{controls.back(),travel.Home,40,travel.Danger,false};
        request.SurfaceCorridor = true; request.QueryDestination = controls.back(); request.DangerRadius = travel.Clearance;
        request.Polyline = RecoveryMovement::SurfacePolyline{travel.Guid,travel.PhaseMask,travel.Goal,controls};
        return request;
    }
    LivingSupply::TravelContext Travel(std::vector<ActionPosition> const& controls)
    {
        LivingSupply::TravelContext travel;
        travel.Begin(ObjectGuid::Create<HighGuid::Unit>(11328,902006),controls.front(),P(8,8),controls.front(),std::nullopt,8,1,1,true);
        travel.Route.Planned.assign(controls.begin()+1,controls.end());
        travel.Route.SurfaceCorridor = true;
        return travel;
    }
}

TEST_CASE("Owned fine execution preserves a physical L corner that its endpoint chord cannot cross", "[AIWorld][RecoveryNavigation][LivingFineRoute]")
{
    auto controls = Corner();
    auto ray = [](ActionPosition const& a,ActionPosition const& b)
    {
        for (unsigned n=0;n<=20;++n)
        {
            float t=float(n)/20,x=a.X+(b.X-a.X)*t,y=a.Y+(b.Y-a.Y)*t;
            if (!((x>=-.2f && x<=1.45f && std::abs(y)<=.2f) ||
                (std::abs(x-1.25f)<=.2f && y>=-.2f && y<=1.45f))) return false;
        }
        return true;
    };
    auto body = [&](ActionPosition const& a,ActionPosition const& b)
    { return LivingSurfaceCorridor::BodyClear(a,b,.1f,2.031f,ray); };
    REQUIRE_FALSE(body(controls.front(),controls.back()));
    LivingSurfaceCorridor::Search chord;
    REQUIRE(LivingSurfaceCorridor::Advance(chord,controls.front(),controls.back(),flat,body,inside,12,tile) == LivingSurfaceCorridor::Status::Rejected);
    auto dense = LivingFineRoute::Revalidate(controls.front(),controls,flat,body,inside,tile);
    REQUIRE_FALSE(dense.empty());
    for (auto const& corner : controls)
        REQUIRE(std::any_of(dense.begin(),dense.end(),[&](auto const& point) { return LivingFineRoute::Same(point,corner); }));
    for (std::size_t n=1;n<dense.size();++n) REQUIRE(body(dense[n-1],dense[n]));
}

TEST_CASE("Fine stair execution resolves the floor on every segment and rejects changed support", "[AIWorld][RecoveryNavigation][LivingFineRoute]")
{
    std::vector<ActionPosition> stairs{P(0,0),P(.625f,0,.4f),P(.625f,.625f,.6f),P(1.25f,.625f,1)};
    auto floor = [](ActionPosition const& p) -> std::optional<float> { return .64f*p.X+.32f*p.Y; };
    auto dense = LivingFineRoute::Revalidate(stairs.front(),stairs,floor,clear,inside,tile);
    REQUIRE_FALSE(dense.empty());
    for (auto const& p : dense) REQUIRE(p.Z == Approx(*floor(p)).margin(.00001f));
    auto missing = [&](ActionPosition const& p) -> std::optional<float>
    { if (p.X>.2f && p.X<.5f) return std::nullopt; return floor(p); };
    REQUIRE(LivingFineRoute::Revalidate(stairs.front(),stairs,missing,clear,inside,tile).empty());
    auto cliff = [&](ActionPosition const& p) -> std::optional<float>
    { return floor(p).value()+((p.X>.2f && p.X<.5f) ? 2.0f : 0.0f); };
    REQUIRE(LivingFineRoute::Revalidate(stairs.front(),stairs,cliff,clear,inside,tile).empty());
}

TEST_CASE("Fine route rechecks interior body tiles bounds and danger rather than only endpoints", "[AIWorld][RecoveryNavigation][LivingFineRoute]")
{
    auto controls=Corner();
    auto forbidden = [](ActionPosition const& p) { return !(p.X>.2f && p.X<.5f && p.Y<.1f); };
    auto body = [&](ActionPosition const& a,ActionPosition const& b) { return forbidden(a)&&forbidden(b); };
    REQUIRE(LivingFineRoute::Revalidate(controls.front(),controls,flat,body,inside,tile).empty());
    REQUIRE(LivingFineRoute::Revalidate(controls.front(),controls,flat,clear,forbidden,tile).empty());
    REQUIRE(LivingFineRoute::Revalidate(controls.front(),controls,flat,clear,inside,forbidden).empty());
    auto danger = [](ActionPosition const& a,ActionPosition const& b)
    { return LivingRolePolicy::AvoidsDanger(a.X,a.Y,b.X,b.Y,.3125f,0,.1f); };
    REQUIRE(LivingFineRoute::Revalidate(controls.front(),controls,flat,danger,inside,tile).empty());
}

TEST_CASE("Owned polyline resume keeps all future bends and may finish its proved sub-yard tail", "[AIWorld][RecoveryNavigation][LivingFineRoute]")
{
    auto controls=Corner();
    auto remaining=LivingFineRoute::Remaining(P(.3f,0),controls);
    REQUIRE(remaining.size()==controls.size());
    REQUIRE(LivingFineRoute::Same(remaining.front(),P(.3f,0)));
    REQUIRE(LivingFineRoute::Same(remaining[2],P(1.25f,0)));
    auto dense=LivingFineRoute::Revalidate(P(1.25f,.8f),controls,flat,clear,inside,tile);
    REQUIRE_FALSE(dense.empty());
    REQUIRE(LivingFineRoute::Distance(dense.front(),dense.back())<1);
    REQUIRE(LivingFineRoute::Revalidate(P(.3f,.2f),controls,flat,clear,inside,tile).empty());
    auto wrongMap=P(.3f,0);wrongMap.MapId=0;
    REQUIRE(LivingFineRoute::Remaining(wrongMap,controls).empty());
}

TEST_CASE("Stair speed resume follows the launched dense floor and cannot forge a missing corner", "[AIWorld][RecoveryNavigation][LivingFineRoute]")
{
    std::vector<ActionPosition> controls{P(0,0),P(.625f,0,.4f),P(1.25f,0,.4f),P(1.25f,.625f,.4f)};
    auto floor=[](ActionPosition const& p)->std::optional<float>{return p.X>.3f ? .4f : 0.0f;};
    auto launched=LivingFineRoute::Revalidate(controls.front(),controls,floor,clear,inside,tile);
    REQUIRE_FALSE(launched.empty());
    REQUIRE(LivingFineRoute::RetainsCorners(controls,launched));
    auto live=P(.3125f,0,.4f);
    REQUIRE(LivingFineRoute::Remaining(live,controls).empty()); // graph chord is not the launched stair surface
    auto resumed=LivingFineRoute::Revalidate(live,launched,floor,clear,inside,tile,nullptr,LivingFineRoute::MaxDensePoints);
    REQUIRE_FALSE(resumed.empty());
    REQUIRE(LivingFineRoute::Same(resumed.back(),launched.back()));
    REQUIRE(std::any_of(resumed.begin(),resumed.end(),[](auto const& p){return LivingFineRoute::Same(p,P(1.25f,0,.4f));}));
    auto tail=LivingFineRoute::Revalidate(P(1.25f,.4f,.4f),launched,floor,clear,inside,tile,nullptr,LivingFineRoute::MaxDensePoints);
    REQUIRE_FALSE(tail.empty());REQUIRE(LivingFineRoute::Distance(tail.front(),tail.back())<1);
    auto travel=Travel(controls);auto request=Request(travel,controls);request.Polyline->SplinePoints=launched;
    REQUIRE(travel.PolylinePrefix(request).has_value());
    auto original=request;
    request.Polyline->SplinePoints={launched.front(),launched.back()};
    REQUIRE_FALSE(LivingFineRoute::RetainsCorners(controls,request.Polyline->SplinePoints));
    REQUIRE_FALSE(request==original);REQUIRE_FALSE(travel.PolylinePrefix(request).has_value());
    request=original;request.Polyline->SplinePoints[1].Z+=.2f;
    REQUIRE_FALSE(request==original); // every dense control belongs to the approved request identity
    travel.Pending=original;
    REQUIRE_FALSE(*travel.Pending==request); // retained callback must match the exact pending launch proof
}

TEST_CASE("Batch lookahead leaves no unstarted sub-yard tail around the final corner", "[AIWorld][RecoveryNavigation][LivingFineRoute]")
{
    std::vector<ActionPosition> planned;
    for(unsigned n=1;n<=9;++n)planned.push_back(P(float(n)*.625f,0));
    planned.push_back(P(5.625f,.625f));
    auto first=LivingFineRoute::Batch(P(0,0),planned);
    REQUIRE(LivingFineRoute::Same(first.back(),P(4.375f,0)));
    auto count=first.size()-1;
    std::vector<ActionPosition> rest(planned.begin()+count,planned.end());
    auto last=LivingFineRoute::Batch(first.back(),rest);
    REQUIRE(LivingFineRoute::Same(last.back(),planned.back()));
    REQUIRE(LivingFineRoute::Bounded(first));REQUIRE(LivingFineRoute::Bounded(last));
    REQUIRE(std::any_of(last.begin(),last.end(),[](auto const& p){return LivingFineRoute::Same(p,P(5.625f,0));}));
    planned.clear();for(unsigned n=1;n<=7;++n)planned.push_back(P(float(n)*.625f,0));
    planned.push_back(P(4.375f,.25f));
    auto all=LivingFineRoute::Batch(P(0,0),planned);
    REQUIRE(LivingFineRoute::Same(all.back(),planned.back()));
}

TEST_CASE("Fine batches are finite bounded and do not lower the generic useful-step threshold", "[AIWorld][RecoveryNavigation][LivingFineRoute]")
{
    REQUIRE(LivingFineRoute::Batch(P(0,0),{P(.625f,0)}).empty());
    REQUIRE_FALSE(LivingReturnPolicy::UsefulStep(P(0,0),P(.625f,0)));
    auto controls=Corner();controls[1].X=std::numeric_limits<float>::quiet_NaN();
    unsigned called=0;auto floor=[&](ActionPosition const&)->std::optional<float>{++called;return 0.0f;};
    REQUIRE(LivingFineRoute::Revalidate(P(0,0),controls,floor,clear,inside,tile).empty());
    REQUIRE(called==0);
    controls={P(0,0),P(6.1f,0)};REQUIRE_FALSE(LivingFineRoute::Bounded(controls));
    controls.clear();for(unsigned n=0;n<18;++n)controls.push_back(P(float(n)*.1f,0));
    REQUIRE_FALSE(LivingFineRoute::Bounded(controls));
    std::vector<ActionPosition> planned;for(unsigned n=1;n<=30;++n)planned.push_back(P(float(n)*.625f,0));
    auto batch=LivingFineRoute::Batch(P(0,0),planned);
    REQUIRE(batch.size()<=LivingFineRoute::MaxControls);REQUIRE(LivingFineRoute::Bounded(batch));
    called=0;REQUIRE_FALSE(LivingFineRoute::Revalidate(P(0,0),batch,floor,clear,inside,tile).empty());
    REQUIRE(called<=64);
    auto curvedFloor=[](ActionPosition const& p)->std::optional<float>
    { return unsigned(std::round(p.X/.3125f))%2 ? .2f : 0.0f; };
    auto dense=LivingFineRoute::Revalidate(P(0,0),batch,curvedFloor,clear,inside,tile);
    REQUIRE_FALSE(dense.empty());
    float length=0;for(std::size_t n=1;n<dense.size();++n)length+=LivingFineRoute::Distance(dense[n-1],dense[n]);
    REQUIRE(length<=LivingFineRoute::MaxLength);
    std::vector<ActionPosition> oldLong{P(0,0)};oldLong.insert(oldLong.end(),planned.begin(),planned.begin()+9);
    REQUIRE(LivingFineRoute::Revalidate(P(0,0),oldLong,curvedFloor,clear,inside,tile).empty());
    auto changedFloor=[](ActionPosition const& p)->std::optional<float>
    { return unsigned(std::round(p.X/.3125f))%2 ? .7f : 0.0f; };
    char const* failure="NONE";
    REQUIRE(LivingFineRoute::Revalidate(P(0,0),batch,changedFloor,clear,inside,tile,&failure).empty());
    REQUIRE(std::string(failure)=="FINE_ROUTE_RANGE");
}

TEST_CASE("Owned arrival consumes the exact entire batch prefix and preserves the unexecuted route", "[AIWorld][RecoveryNavigation][LivingFineRoute]")
{
    auto controls=Corner();auto travel=Travel(controls);
    travel.Route.Planned.push_back(P(1.875f,1.25f));
    auto request=Request(travel,controls);
    REQUIRE(travel.PolylinePrefix(request)==controls.size()-1);
    travel.Pending=request;travel.PendingFrom=controls.front();
    REQUIRE(travel.ObserveSource(controls.back(),true));
    REQUIRE_FALSE(travel.Pending.has_value());REQUIRE(travel.Route.Planned.size()==1);
    REQUIRE(LivingFineRoute::Same(travel.Route.Planned.front(),P(1.875f,1.25f)));
    REQUIRE(travel.Route.SurfaceCorridor);
    REQUIRE(LivingFineRoute::Same(travel.Origin,controls.back()));
    REQUIRE_FALSE(travel.Search.HasContext);
}

TEST_CASE("Polyline ownership rejects changed GUID phase goal home corner and premature arrival", "[AIWorld][RecoveryNavigation][LivingFineRoute]")
{
    auto controls=Corner();auto travel=Travel(controls);auto request=Request(travel,controls);
    auto original=request;
    request.Polyline->Owner=ObjectGuid::Create<HighGuid::Unit>(11328,902007);
    REQUIRE_FALSE(request==original);REQUIRE_FALSE(travel.PolylinePrefix(request));request=original;
    ++request.Polyline->PhaseMask;REQUIRE_FALSE(request==original);REQUIRE_FALSE(travel.PolylinePrefix(request));request=original;
    request.Polyline->Goal=P(9,8);REQUIRE_FALSE(request==original);REQUIRE_FALSE(travel.PolylinePrefix(request));request=original;
    request.Home=P(0,1);REQUIRE_FALSE(travel.PolylinePrefix(request));request=original;
    request.Polyline->Points[1]=P(.625f,.1f);REQUIRE_FALSE(request==original);REQUIRE_FALSE(travel.PolylinePrefix(request));request=original;
    travel.Pending=request;travel.PendingFrom=controls.front();
    REQUIRE_FALSE(travel.ObserveSource(controls.front(),true));
    REQUIRE_FALSE(travel.ObserveSource(P(1.25f,.9f),true));
    REQUIRE(travel.Route.Planned.size()==controls.size()-1);
    travel.Route.Planned[1]=P(1.25f,.1f);
    REQUIRE_FALSE(travel.ObserveSource(controls.back(),true));
    REQUIRE(travel.Route.Planned.size()==controls.size()-1);
}

TEST_CASE("Legacy single-leg arrival remains independent of resident fine-route ownership", "[AIWorld][RecoveryNavigation][LivingFineRoute]")
{
    LivingSupply::TravelContext travel;
    auto guid=ObjectGuid::Create<HighGuid::Unit>(11328,901001);
    travel.Begin(guid,P(0,0),P(10,0),P(0,0),std::nullopt,8,1,1);
    travel.Route.Planned={P(2.5f,0),P(5,0)};
    travel.Pending=RecoveryMovement{P(2.5f,0),P(0,0),40,std::nullopt,false};travel.PendingFrom=P(0,0);
    REQUIRE_FALSE(travel.Pending->Polyline.has_value());
    REQUIRE(travel.ObserveSource(P(2.5f,0),true));
    REQUIRE(travel.Route.Planned.size()==1);REQUIRE(LivingFineRoute::Same(travel.Route.Planned.front(),P(5,0)));
}
