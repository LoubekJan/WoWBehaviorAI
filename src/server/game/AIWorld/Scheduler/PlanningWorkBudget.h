/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_PLANNINGWORKBUDGET_H
#define AIWORLD_PLANNINGWORKBUDGET_H

#include "PlanningWorkScheduler.h"
#include <chrono>
#include <cstdint>
#include <utility>

// Charge synchronous query bodies, not unrelated needs/sensing wall time.
// An actor gets a bounded slice after fair admission. The caller must retain
// its search cursor on refusal and keep safety/needs/motion maintenance live.
// Gate bounded leaf work, never a whole multi-candidate search.
class PlanningWorkBudget
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    struct Statistics
    {
        std::uint64_t Started = 0, Deferred = 0;
        std::uint64_t OperationUs = 0, MaxOperationUs = 0;
    };

    // The optional timestamp keeps old callers source-compatible; only actual
    // permit work is billed, so creating the budget starts no wall deadline.
    explicit PlanningWorkBudget(std::chrono::microseconds duration, std::uint32_t operations,
        TimePoint = {}) : _duration(duration), _operations(operations) { }
    PlanningWorkBudget(PlanningWorkBudget const&) = delete;
    PlanningWorkBudget& operator=(PlanningWorkBudget const&) = delete;

    class Scope
    {
    public:
        explicit Scope(PlanningWorkBudget& budget) : _previous(_active) { _active = &budget; }
        ~Scope() { _active = _previous; }
        Scope(Scope const&) = delete;
        Scope& operator=(Scope const&) = delete;
    private:
        PlanningWorkBudget* _previous;
    };

    // Attach once around an actor's needs update. This scope is not a query
    // permit and bills no maintenance time. Request() precedes Available().
    class ActorScope
    {
    public:
        explicit ActorScope(PlanningWorkScheduler& scheduler, AgentId id,
            std::chrono::microseconds duration = std::chrono::microseconds(500), std::uint32_t operations = 4) :
            _scheduler(scheduler), _id(id), _budget(_active), _previous(_actor), _duration(duration), _operations(operations)
        { _actor = this; }
        ~ActorScope()
        {
            _actor = _previous;
            _scheduler.EndActor(_id, _started != 0);
        }
        ActorScope(ActorScope const&) = delete;
        ActorScope& operator=(ActorScope const&) = delete;
        std::uint64_t StartedOperations() const { return _started; }
        std::uint64_t OperationUs() const
        { return std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(_spent).count()); }
    private:
        friend class PlanningWorkBudget;
        bool CanStart() const
        { return _started < _operations && _spent < _duration && _scheduler.CanAdmit(_id); }
        PlanningWorkScheduler& _scheduler;
        AgentId _id;
        PlanningWorkBudget* _budget;
        ActorScope* _previous;
        std::chrono::microseconds _duration;
        Clock::duration _spent{};
        std::uint32_t _operations;
        std::uint64_t _started = 0;
    };

    class Permit
    {
    public:
        Permit(Permit const&) = delete;
        Permit& operator=(Permit const&) = delete;
        Permit(Permit&& other) noexcept : _budget(std::exchange(other._budget, nullptr)),
            _allowed(std::exchange(other._allowed, false)) { }
        Permit& operator=(Permit&& other) noexcept
        {
            if (this != &other)
            {
                Finish();
                _budget = std::exchange(other._budget, nullptr);
                _allowed = std::exchange(other._allowed, false);
            }
            return *this;
        }
        ~Permit() { Finish(); }
        explicit operator bool() const { return _allowed; }
        // All permits must finish before their actor/global scopes end.
        void Finish(TimePoint now = Clock::now())
        {
            if (!_budget) return;
            _budget->FinishQuery(now);
            _budget = nullptr;
        }
    private:
        friend class PlanningWorkBudget;
        Permit(PlanningWorkBudget* budget, bool allowed) : _budget(budget), _allowed(allowed) { }
        PlanningWorkBudget* _budget;
        bool _allowed;
    };

    static bool Available(TimePoint = Clock::now())
    { return !_active || _active->CanStart(); }

    [[nodiscard]] static Permit TryAcquire(TimePoint now = Clock::now())
    {
        if (!_active) return Permit(nullptr, true);
        if (!_active->CanStart())
        {
            ++_active->_statistics.Deferred;
            return Permit(nullptr, false);
        }
        if (_active->_queryDepth)
        {
            // Reentrant leaf work shares its already admitted outer query.
            // No second actor/op charge and no double-counted duration.
            ++_active->_queryDepth;
            return Permit(_active, true);
        }
        auto actor = _active->CurrentActor();
        if (actor)
        {
            if (!actor->_scheduler.TryAdmit(actor->_id))
            {
                ++_active->_statistics.Deferred;
                return Permit(nullptr, false);
            }
            ++actor->_started;
        }
        ++_active->_statistics.Started;
        _active->_queryDepth = 1;
        _active->_queryStart = now;
        _active->_queryActor = actor;
        return Permit(_active, true);
    }

    static void MarkDeferred() { if (_active) ++_active->_statistics.Deferred; }
    static std::uint64_t StartedOperations() { return _active ? _active->_statistics.Started : 0; }
    Statistics const& GetStatistics() const { return _statistics; }

private:
    ActorScope* CurrentActor() const { return _actor && _actor->_budget == this ? _actor : nullptr; }
    bool CanStart() const
    {
        if (_queryDepth) return CurrentActor() == _queryActor;
        auto actor = CurrentActor();
        return _spent < _duration && _statistics.Started < _operations && (!actor || actor->CanStart());
    }
    void FinishQuery(TimePoint now)
    {
        if (--_queryDepth) return;
        auto elapsed = now >= _queryStart ? now - _queryStart : Clock::duration::zero();
        _spent += elapsed;
        _statistics.OperationUs = std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(_spent).count());
        auto elapsedUs = std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
        if (elapsedUs > _statistics.MaxOperationUs) _statistics.MaxOperationUs = elapsedUs;
        if (_queryActor) _queryActor->_spent += elapsed;
        _queryActor = nullptr;
    }
    inline static thread_local PlanningWorkBudget* _active = nullptr;
    inline static thread_local ActorScope* _actor = nullptr;
    std::chrono::microseconds _duration;
    std::uint32_t _operations;
    Clock::duration _spent{};
    TimePoint _queryStart{};
    unsigned _queryDepth = 0;
    ActorScope* _queryActor = nullptr;
    Statistics _statistics;
};
#endif
