/* This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 * Licensed under the GNU General Public License, version 2 or later. */
#ifndef AIWORLD_LIVINGRETURNADVICE_H
#define AIWORLD_LIVINGRETURNADVICE_H
#include <utility>

// One returning actor's decision order. Engine callbacks still own admission,
// geometry and suspended search state; this runner never resets either search.
namespace LivingReturnAdvice
{
    enum class Outcome { NoStep, ReturnStep, AdviceStep, Deferred, Pending, RetryWait };
    struct Context
    {
        bool Pending = false, SearchActive = false, Offered = false, RetryDue = true;
    };

    template<class Callback>
    class OnceAttempt
    {
        Callback _callback;
        bool _tried = false;
        Outcome _result = Outcome::NoStep;
    public:
        explicit OnceAttempt(Callback callback) : _callback(std::move(callback)) { }
        bool Tried() const { return _tried; }
        Outcome operator()()
        {
            if (!_tried) { _tried = true; _result = _callback(); }
            return _result;
        }
    };
    template<class Callback> OnceAttempt(Callback) -> OnceAttempt<Callback>;

    inline bool PreferAdvice(Context const& context)
    { return context.Pending || context.SearchActive || context.Offered; }

    template<class AdviceAttempt>
    Outcome TryPriority(Context const& context, AdviceAttempt&& tryAdvice)
    { return PreferAdvice(context) ? tryAdvice() : Outcome::NoStep; }

    template<class ReturnAttempt, class AdviceAttempt>
    Outcome Select(Context const& context, ReturnAttempt&& tryReturn, AdviceAttempt&& tryAdvice)
    {
        bool adviceFirst = PreferAdvice(context);
        bool searchedReturn = false;
        if (!adviceFirst && context.RetryDue)
        {
            searchedReturn = true;
            auto result = tryReturn();
            if (result != Outcome::NoStep) return result;
        }
        auto result = tryAdvice();
        if (result != Outcome::NoStep) return result;
        if (!context.RetryDue) return Outcome::RetryWait;
        return searchedReturn ? Outcome::NoStep : tryReturn();
    }
}
#endif
