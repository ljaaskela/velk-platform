#ifndef VELK_RENDER_EXT_GPU_RESOURCE_H
#define VELK_RENDER_EXT_GPU_RESOURCE_H

#include <velk/ext/object.h>
#include <velk/vector.h>

#include <velk-render/interface/intf_gpu_resource.h>

#include <mutex>

namespace velk {
namespace ext {

/**
 * @brief CRTP base for objects that own GPU handles and need lifetime
 *        observation.
 *
 * Inherits from `ext::Object<FinalClass, Interfaces...>` so concrete
 * resource classes (textures, buffers, future BVH nodes, etc.) get the
 * full Object machinery in one base. Provides the `IGpuResource` observer
 * list, the `add_gpu_resource_observer` / `remove_gpu_resource_observer`
 * implementations, and an automatic destructor that notifies all
 * observers before the object's memory is reclaimed.
 *
 * The interface chain must include something that derives from
 * `IGpuResource` (e.g. `ITexture`, `IBuffer`); otherwise the compiler
 * rejects the implicit cast in the destructor's notify call.
 *
 * Usage:
 * ```cpp
 * class MyTexture : public ext::GpuResource<MyTexture, ITexture, IMyExtra>
 * {
 *     // No need to declare add_/remove_gpu_resource_observer or a
 *     // destructor: the GpuResource base handles both.
 *     int width() const override { ... }
 *     // ...
 * };
 * ```
 *
 * The observer callback receives `this` cast to `IGpuResource*`. Observers
 * must key off the pointer value only and must not access any per-instance
 * state through it: by the time the GpuResource destructor runs, the
 * derived class's destructor has already finished and its members are
 * gone. Observers should do minimal, lock-protected work in their
 * callbacks (e.g. push to a deferred-destruction queue).
 */
template <class FinalClass, class... Interfaces>
class GpuResource : public ::velk::ext::Object<FinalClass, Interfaces...>
{
public:
    ~GpuResource() override
    {
        // Snapshot under the lock, notify outside it so observer callbacks
        // can safely re-enter (e.g. remove themselves) without deadlocking.
        vector<IGpuResourceObserver*> snapshot;
        {
            std::lock_guard<std::mutex> lock(gpu_observers_mutex_);
            snapshot = gpu_observers_;
            gpu_observers_.clear();
        }
        // The implicit upcast from `this` to IGpuResource* requires that
        // FinalClass's interface chain includes IGpuResource (typically via
        // ITexture or IBuffer). If you see a compile error here, your
        // class is using GpuResource without inheriting an IGpuResource
        // interface, which is a misuse.
        IGpuResource* self = this;
        for (auto* obs : snapshot) {
            obs->on_gpu_resource_destroyed(self);
        }
    }

    /// Adds an observer. Idempotent (adding twice yields one notification).
    void add_gpu_resource_observer(IGpuResourceObserver* obs) override
    {
        if (!obs) {
            return;
        }
        std::lock_guard<std::mutex> lock(gpu_observers_mutex_);
        for (auto* existing : gpu_observers_) {
            if (existing == obs) {
                return;
            }
        }
        gpu_observers_.push_back(obs);
    }

    /// Removes an observer. Safe if the observer was never added.
    void remove_gpu_resource_observer(IGpuResourceObserver* obs) override
    {
        if (!obs) {
            return;
        }
        std::lock_guard<std::mutex> lock(gpu_observers_mutex_);
        for (size_t i = 0; i < gpu_observers_.size(); ++i) {
            if (gpu_observers_[i] == obs) {
                gpu_observers_.erase(gpu_observers_.begin() + i);
                return;
            }
        }
    }

private:
    vector<IGpuResourceObserver*> gpu_observers_;
    mutable std::mutex gpu_observers_mutex_;
};

} // namespace ext
} // namespace velk

#endif // VELK_RENDER_EXT_GPU_RESOURCE_H
