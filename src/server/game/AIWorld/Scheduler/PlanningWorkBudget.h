/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_PLANNINGWORKBUDGET_H
#define AIWORLD_PLANNINGWORKBUDGET_H

#include <chrono>
#include <cstdint>
#include <utility>

// Cooperative world-thread budget for synchronous planning. Gate individual
// candidates, not an entire search: callers retain their cursor when deferred.
// A refused permit is not a navigation failure. Existing motion, needs and
// threat handling must continue without requiring a permit.
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

    explicit PlanningWorkBudget(std::chrono::microseconds duration, std::uint32_t operations,
        TimePoint started = Clock::now()) : _started(started), _duration(duration), _operations(operations) { }

    PlanningWorkBudget(PlanningWorkBudget const&) = delete;
    PlanningWorkBudget& operator=(PlanningWorkBudget const&) = delete;

    // Only the needs update attaches a scope. Engine chase refreshes and manual
    // navigation probes outside it keep their ordinary synchronous semantics.
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

    class Permit
    {
    public:
        Permit(Permit const&) = delete;
        Permit& operator=(Permit const&) = delete;
        Permit(Permit&& other) noexcept : _budget(std::exchange(other._budget, nullptr)),
            _started(other._started), _allowed(std::exchange(other._allowed, false)) { }
        Permit& operator=(Permit&& other) noexcept
        {
            if (this != &other)
            {
                Finish();
                _budget = std::exchange(other._budget, nullptr);
                _started = other._started;
                _allowed = std::exchange(other._allowed, false);
            }
            return *this;
        }
        ~Permit() { Finish(); }
        explicit operator bool() const { return _allowed; }

        // Explicit completion also permits deterministic tests with supplied
        // steady-clock timestamps. Normal callers rely on the destructor.
        void Finish(TimePoint now = Clock::now())
        {
            if (!_budget) return;
            auto elapsed = now >= _started ? std::chrono::duration_cast<std::chrono::microseconds>(now - _started).count() : 0;
            auto elapsedUs = std::uint64_t(elapsed);
            _budget->_statistics.OperationUs += elapsedUs;
            if (elapsedUs > _budget->_statistics.MaxOperationUs) _budget->_statistics.MaxOperationUs = elapsedUs;
            _budget = nullptr;
        }
    private:
        friend class PlanningWorkBudget;
        Permit(PlanningWorkBudget* budget, TimePoint started, bool allowed) :
            _budget(budget), _started(started), _allowed(allowed) { }
        PlanningWorkBudget* _budget;
        TimePoint _started;
        bool _allowed;
    };

    static bool Available(TimePoint now = Clock::now())
    { return !_active || _active->CanStart(now); }

    [[nodiscard]] static Permit TryAcquire(TimePoint now = Clock::now())
    {
        if (!_active) return Permit(nullptr, now, true);
        if (!_active->CanStart(now))
        {
            ++_active->_statistics.Deferred;
            return Permit(nullptr, now, false);
        }
        ++_active->_statistics.Started;
        return Permit(_active, now, true);
    }

    // A top-level availability check does not consume a candidate. Count a
    // decision deliberately deferred there once, without double-counting a
    // subsequent refused TryAcquire at the same leaf.
    static void MarkDeferred() { if (_active) ++_active->_statistics.Deferred; }
    static std::uint64_t StartedOperations() { return _active ? _active->_statistics.Started : 0; }
    Statistics const& GetStatistics() const { return _statistics; }

private:
    bool CanStart(TimePoint now) const
    {
        return now >= _started && now - _started < _duration &&
            _statistics.Started < _operations;
    }
    inline static thread_local PlanningWorkBudget* _active = nullptr;
    TimePoint _started;
    std::chrono::microseconds _duration;
    std::uint32_t _operations;
    Statistics _statistics;
};

#endif
