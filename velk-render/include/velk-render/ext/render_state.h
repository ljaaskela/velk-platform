#ifndef VELK_RENDER_EXT_RENDER_STATE_H
#define VELK_RENDER_EXT_RENDER_STATE_H

#include <velk/ext/core_object.h>
#include <velk/vector.h>

#include <velk-render/interface/intf_render_state.h>

#include <mutex>

namespace velk::ext {

/**
 * @brief CRTP base for objects that publish state-change events through
 *        `IRenderState`.
 *
 * Use as the inheritance base in place of `ext::ObjectCore`; the
 * `IRenderState` virtuals are implemented here. Derived classes call
 * `notify_render_state_changed(flags)` after mutating their state.
 *
 * ```cpp
 * class MyState : public ext::RenderState<MyState, IMyState>
 * {
 *     void mutate()
 *     {
 *         // ... change state ...
 *         this->notify_render_state_changed(RenderStateChange::All);
 *     }
 * };
 * ```
 *
 * Thread-safe. Add / remove / notify may be called from any thread.
 * Observer callbacks may call `remove_render_state_observer` on the
 * source they were notified from; re-entrant `add` from within a
 * callback is not supported.
 */
template <class FinalClass, class... Interfaces>
class RenderState
    : public ::velk::ext::ObjectCore<FinalClass, Interfaces...>
{
public:
    void add_render_state_observer(IRenderStateObserver* obs) override
    {
        if (!obs) return;
        std::lock_guard<std::mutex> lock(observers_mutex_);
        if (solo_ == obs) return;
        for (auto* existing : overflow_) {
            if (existing == obs) return;
        }
        if (!solo_) {
            solo_ = obs;
        } else {
            overflow_.push_back(obs);
        }
    }

    void remove_render_state_observer(IRenderStateObserver* obs) override
    {
        if (!obs) return;
        std::lock_guard<std::mutex> lock(observers_mutex_);
        if (solo_ == obs) {
            // Promote first overflow entry into the solo slot to keep
            // the fast path populated when possible.
            if (!overflow_.empty()) {
                solo_ = overflow_[0];
                overflow_.erase(overflow_.begin());
            } else {
                solo_ = nullptr;
            }
            return;
        }
        for (size_t i = 0; i < overflow_.size(); ++i) {
            if (overflow_[i] == obs) {
                overflow_.erase(overflow_.begin() + i);
                return;
            }
        }
    }

protected:
    /// Snapshot observers under the lock and notify outside it. `this`
    /// is resolved to `IRenderState*` via `get_interface` to handle any
    /// future diamond cleanly.
    void notify_render_state_changed(RenderStateChange flags)
    {
        IRenderStateObserver* solo_snapshot = nullptr;
        ::velk::vector<IRenderStateObserver*> overflow_snapshot;
        {
            std::lock_guard<std::mutex> lock(observers_mutex_);
            solo_snapshot = solo_;
            if (!overflow_.empty()) {
                overflow_snapshot = overflow_;
            }
        }
        IRenderState* self = this->template get_interface<IRenderState>();
        if (solo_snapshot) {
            solo_snapshot->on_render_state_changed(self, flags);
        }
        for (auto* obs : overflow_snapshot) {
            obs->on_render_state_changed(self, flags);
        }
    }

private:
    /// First observer lives inline — zero allocation for the
    /// universally-common 0/1 case.
    IRenderStateObserver* solo_ = nullptr;
    /// Populated only on the second add. Once non-empty, removal of
    /// `solo_` promotes overflow_[0] to keep the fast path warm.
    ::velk::vector<IRenderStateObserver*> overflow_;
    mutable std::mutex observers_mutex_;
};

} // namespace velk::ext

#endif // VELK_RENDER_EXT_RENDER_STATE_H
