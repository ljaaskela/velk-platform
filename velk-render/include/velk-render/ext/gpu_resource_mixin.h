#ifndef VELK_RENDER_EXT_GPU_RESOURCE_MIXIN_H
#define VELK_RENDER_EXT_GPU_RESOURCE_MIXIN_H

#include <velk-render/interface/intf_gpu_resource.h>
#include <velk/vector.h>

#include <mutex>

namespace velk {
namespace ext {

/**
 * @brief Mixin providing observer-list management for IGpuResource impls.
 *
 * Concrete classes that implement `IGpuResource` (or any subtype like
 * `ITexture`) inherit from this mixin to get a thread-safe observer list
 * and the standard add/remove methods. They must call
 * `notify_gpu_resource_destroyed()` from their destructor, passing
 * themselves, so observers receive a notification before the object's
 * memory is reclaimed.
 *
 * Usage:
 * ```cpp
 * class MyTexture : public ext::Object<MyTexture, ITexture>,
 *                   public ext::GpuResourceMixin
 * {
 * public:
 *     ~MyTexture() { notify_gpu_resource_destroyed(this); }
 *
 *     // ITexture / IGpuResource overrides forward to the mixin:
 *     void add_gpu_resource_observer(IGpuResourceObserver* o) override
 *         { GpuResourceMixin::add_observer(o); }
 *     void remove_gpu_resource_observer(IGpuResourceObserver* o) override
 *         { GpuResourceMixin::remove_observer(o); }
 *     // ...
 * };
 * ```
 *
 * The mixin is intentionally not a CRTP base because it does not need to
 * know the concrete type; the concrete dtor passes `this` to the notify
 * helper, which is sufficient for observers that key off the
 * `IGpuResource*` pointer.
 */
class GpuResourceMixin
{
public:
    /// Adds an observer. Idempotent (adding twice yields one notification).
    void add_observer(IGpuResourceObserver* obs)
    {
        if (!obs) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto* existing : observers_) {
            if (existing == obs) {
                return;
            }
        }
        observers_.push_back(obs);
    }

    /// Removes an observer. Safe if the observer was never added.
    void remove_observer(IGpuResourceObserver* obs)
    {
        if (!obs) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < observers_.size(); ++i) {
            if (observers_[i] == obs) {
                observers_.erase(observers_.begin() + i);
                return;
            }
        }
    }

    /**
     * @brief Notifies all observers that @p resource is being destroyed.
     *        Concrete classes call this from their destructor.
     *
     * Snapshots the observer list under the lock, then notifies outside
     * the lock so observer callbacks may safely call back into the
     * resource (e.g. to remove themselves) without reentrancy.
     */
    void notify_gpu_resource_destroyed(IGpuResource* resource)
    {
        vector<IGpuResourceObserver*> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot = observers_;
            observers_.clear();
        }
        for (auto* obs : snapshot) {
            obs->on_gpu_resource_destroyed(resource);
        }
    }

private:
    vector<IGpuResourceObserver*> observers_;
    mutable std::mutex mutex_;
};

} // namespace ext
} // namespace velk

#endif // VELK_RENDER_EXT_GPU_RESOURCE_MIXIN_H
