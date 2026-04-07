#ifndef VELK_RENDER_INTF_GPU_RESOURCE_H
#define VELK_RENDER_INTF_GPU_RESOURCE_H

#include <velk/interface/intf_interface.h>

namespace velk {

class IGpuResource;

/**
 * @brief Observer of GPU resource lifetime events.
 *
 * Implementations are notified when an observed `IGpuResource` is destroyed,
 * so they can defer destruction of any associated GPU handles until the
 * GPU is provably done with them (typically N frames later, where N is the
 * frames-in-flight count).
 *
 * Chain: IInterface -> IGpuResourceObserver
 */
class IGpuResourceObserver : public Interface<IGpuResourceObserver>
{
public:
    /**
     * @brief Called from the resource's destructor.
     *
     * Synchronous. The pointer @p resource is valid for the duration of
     * this call only; the observer must perform any lookup keyed by it
     * immediately and must not hold the pointer.
     *
     * May be called from any thread (whichever dropped the last strong
     * reference). Implementations must be thread-safe.
     */
    virtual void on_gpu_resource_destroyed(IGpuResource* resource) = 0;
};

/**
 * @brief Base interface for any object that owns one or more GPU handles.
 *
 * GPU resources require careful destruction: a handle being used by an
 * in-flight draw call must not be released until the GPU finishes with it.
 * This is solved by an observer pattern: the renderer (or any GPU-aware
 * subsystem) registers as an observer when it creates a GPU handle for the
 * resource. When the resource's last strong reference drops and its dtor
 * runs, observers are notified synchronously and can enqueue their handles
 * for deferred destruction.
 *
 * Concrete subtypes (`ITexture`, future `IBuffer`, etc.) inherit from this
 * interface and rely on a small mixin (`ext::GpuResourceMixin`) to manage
 * the observer list and notify in their dtor.
 *
 * Lifetime contract:
 * - The owner of a strong reference may drop it on any thread, at any time.
 * - The resource's dtor runs on whichever thread released last.
 * - Observers must do minimal, lock-protected work in their callbacks
 *   (e.g. push to a deferred-destruction queue).
 * - Observers must NOT outlive the resources they observe without first
 *   calling `remove_gpu_resource_observer()`. The simplest way to achieve
 *   this is for observers to either own the resource (renderer holds the
 *   only weak link) or detach during their own shutdown.
 *
 * Chain: IInterface -> IGpuResource
 */
class IGpuResource : public Interface<IGpuResource>
{
public:
    /**
     * @brief Registers @p obs to be notified when this resource is destroyed.
     *
     * Multiple observers may be registered (e.g. multi-window setups with
     * multiple renderers). Adding the same observer twice is permitted but
     * results in a single notification.
     */
    virtual void add_gpu_resource_observer(IGpuResourceObserver* obs) = 0;

    /**
     * @brief Removes a previously registered observer.
     *
     * Safe to call if the observer was never added.
     */
    virtual void remove_gpu_resource_observer(IGpuResourceObserver* obs) = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_GPU_RESOURCE_H
