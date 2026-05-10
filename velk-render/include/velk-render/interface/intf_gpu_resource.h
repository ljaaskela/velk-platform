#ifndef VELK_RENDER_INTF_GPU_RESOURCE_H
#define VELK_RENDER_INTF_GPU_RESOURCE_H

#include <velk/interface/intf_interface.h>

namespace velk {

/** @brief Discriminator for GPU resource types. */
enum class GpuResourceType : uint8_t
{
    Surface,
    Texture,
    Buffer,
    Program
};

/**
 * @brief Reserved keys for `IGpuResource::get_gpu_handle` /
 *        `set_gpu_handle`. Each resource type defines its own
 *        additional conventions (e.g. attachment indices for
 *        `IRenderTextureGroup`); `Default` is the canonical primary
 *        handle and is always available.
 */
namespace GpuResourceKey {
inline constexpr uint64_t Default = 0;
} // namespace GpuResourceKey

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
 * Concrete subtypes (`ISurface`, `IBuffer`, etc.) inherit from this
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
    /** @brief Returns the resource type discriminator. */
    virtual GpuResourceType get_type() const = 0;

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

    /**
     * @brief True iff at least one observer is registered.
     *
     * Concrete resources branch on this in their destructor:
     * managed resources let the observer (typically the
     * `GpuResourceManager`) drive destruction; unmanaged ones
     * (allocated directly via the backend, bypassing the manager)
     * free themselves.
     */
    virtual bool is_managed() const = 0;

    /**
     * @brief Returns a backend handle for this resource keyed by @p key.
     *
     * `GpuResourceKey::Default` (0) is the resource's primary handle —
     * for surfaces / render targets the bind handle (passed to
     * `begin_pass`), for buffers the GPU device address, for programs
     * the pipeline handle. Non-zero keys retrieve alternates:
     * per-attachment ids for a multi-attachment group, per-frame-slot
     * rotated handles, format variants, aliased transient backings.
     *
     * Each implementation defines its own key conventions. Returns 0
     * when @p key has no associated handle on this resource.
     */
    virtual uint64_t get_gpu_handle(uint64_t key) const = 0;

    /**
     * @brief Stores a backend handle for this resource at @p key.
     *
     * Mirrors `get_gpu_handle`. The renderer / resource manager calls
     * this after the backend allocates a handle — e.g. after
     * `register_texture` populates the bindless TextureId, the
     * manager calls `surf->set_gpu_handle(GpuResourceKey::Default, tid)`
     * so subsequent samplers can fetch the id from the resource itself.
     */
    virtual void set_gpu_handle(uint64_t key, uint64_t value) = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_GPU_RESOURCE_H
