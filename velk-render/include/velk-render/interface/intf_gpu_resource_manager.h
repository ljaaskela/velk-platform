#ifndef VELK_RENDER_INTF_GPU_RESOURCE_MANAGER_H
#define VELK_RENDER_INTF_GPU_RESOURCE_MANAGER_H

#include <velk/interface/intf_interface.h>
#include <velk/uid.h>

#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_gpu_resource.h>
#include <velk-render/interface/intf_program.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/interface/intf_render_texture_group.h>
#include <velk-render/interface/intf_texture_resolver.h>
#include <velk-render/plugin.h>
#include <velk-render/render_types.h>

namespace velk {

class ISurface;

/**
 * @brief Tracks GPU resources for a renderer instance.
 *
 * Pipeline-side surface only: factories that produce managed
 * `IGpuResource::Ptr`s, plus the lookup tables consumed during data
 * upload. Lifecycle bookkeeping (deferred-destroy queues, observer
 * forwarding, drain, shutdown) lives behind the framework-internal
 * `IGpuResourceManagerInternal` interface — pipelines never call those
 * directly; the dtor of a managed Ptr drives them automatically.
 *
 * Also provides ISurface->TextureId resolution (sibling base
 * `ITextureResolver`) so materials can embed bindless texture ids in
 * their UBOs via IDrawData::write_draw_data without coupling to the
 * concrete manager.
 */
class IGpuResourceManager
    : public Interface<IGpuResourceManager, IInterface,
                       VELK_UID("04f21ebb-d725-457b-97de-3a8d81262b9f")>,
      public ITextureResolver
{
public:
    /** @brief Backend handle + size pair tracked per CPU IBuffer. */
    struct BufferEntry
    {
        GpuBufferHandle handle{};
        size_t size = 0;
    };

    /// Creates a backend texture, wraps it in a RenderTexture, registers
    /// it for lifecycle tracking, and returns the Ptr. When the last
    /// reference drops, the backend handle is auto-deferred for destroy
    /// using the current `pending_frame_completion_marker()`. Pipelines
    /// and paths use this instead of calling `IRenderBackend::create_texture`
    /// directly — they never see a raw `TextureId`.
    virtual IRenderTarget::Ptr create_render_texture(const TextureDesc& desc) = 0;

    /// Creates a multi-attachment render-target group, wraps it in a
    /// RenderTextureGroup, populates the attachments, and registers it
    /// for lifecycle tracking. Dropping the last Ptr auto-defers the
    /// backend group handle for destroy (which cascades to all its
    /// attachments). Pipelines never see a raw `RenderTargetGroup`.
    virtual IRenderTextureGroup::Ptr create_render_texture_group(
        const TextureGroupDesc& desc) = 0;

    // Texture mapping
    virtual TextureId find_texture(ISurface* surf) const = 0;
    virtual void register_texture(ISurface* surf, TextureId tid) = 0;
    virtual void unregister_texture(ISurface* surf) = 0;

    /// Ensures @p surf has a backend texture allocated and registered.
    /// Returns the existing TextureId on cache hit; allocates fresh +
    /// registers + stamps `set_gpu_handle(Default, ...)` on first sight;
    /// returns 0 on backend allocation failure. The pixel upload itself
    /// remains the caller's responsibility (this only resolves the
    /// backend handle; CPU bytes -> GPU is a separate step).
    virtual TextureId ensure_texture_storage(ISurface* surf,
                                             const TextureDesc& desc) = 0;

    // Buffer mapping
    virtual BufferEntry* find_buffer(IBuffer* buf) = 0;
    virtual void register_buffer(IBuffer* buf, const BufferEntry& entry) = 0;
    virtual void unregister_buffer(IBuffer* buf) = 0;

    /// Ensures @p buf has backend storage matching @p desc.size. Reuses
    /// the existing entry when sizes match; reallocates (deferring the
    /// prior handle for destroy at the current pending frame marker)
    /// when they differ; allocates fresh when none exists. Stamps the
    /// buffer's GPU virtual address into `set_gpu_handle(Default, ...)`.
    /// Returns the resolved BufferEntry* (matching @p desc.size), or
    /// nullptr on backend allocation failure.
    virtual BufferEntry* ensure_buffer_storage(IBuffer* buf,
                                               const GpuBufferDesc& desc) = 0;

    /// Idempotent. Returns true if the program was newly registered,
    /// false if it was already tracked. Subscription happens internally
    /// — callers don't need to add the manager as observer themselves.
    virtual bool register_pipeline(IProgram* prog, PipelineId pid) = 0;

    // Environment resource bookkeeping (textures referenced by env material;
    // not tracked through the element cache so they need a sidecar list).
    // Unsubscription happens automatically inside `shutdown()` on the
    // internal interface.
    virtual void add_env_observer(const IBuffer::WeakPtr& res) = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_GPU_RESOURCE_MANAGER_H
