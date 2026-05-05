#ifndef VELK_UI_GPU_RESOURCE_MANAGER_H
#define VELK_UI_GPU_RESOURCE_MANAGER_H

#include <velk/ext/core_object.h>
#include <velk/vector.h>

#include <unordered_map>
#include <mutex>

#include <velk-render/detail/intf_gpu_resource_manager_internal.h>
#include <velk-render/interface/intf_gpu_resource_manager.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/interface/intf_surface.h>
#include <velk-render/plugin.h>

namespace velk {

/**
 * @brief Concrete IGpuResourceManager backed by std::unordered_map plus
 *        per-resource deferred-destroy queues protected by a mutex.
 *
 * Owned by Renderer as `IGpuResourceManager::Ptr`; instantiated through
 * the velk type registry so the allocation participates in the hive.
 */
class GpuResourceManager
    : public ext::ObjectCore<GpuResourceManager,
                             IGpuResourceManagerInternal,
                             IGpuResourceObserver>
{
public:
    VELK_CLASS_UID(ClassId::GpuResourceManager, "GpuResourceManager");

    ~GpuResourceManager() override;

    // ITextureResolver
    TextureId resolve(ISurface* surf) const override
    {
        if (!surf) return 0;
        TextureId tid = find_texture(surf);
        if (tid == 0) {
            uint64_t rt_id = ::velk::get_render_target_id(surf);
            if (rt_id != 0) tid = static_cast<TextureId>(rt_id);
        }
        return tid;
    }

    // IGpuResourceManager
    void init(IRenderBackend* backend) override;
    void enable_transient_pool() override;
    IRenderTarget::Ptr create_render_texture(const TextureDesc& desc) override;
    IRenderTextureGroup::Ptr create_render_texture_group(
        const TextureGroupDesc& desc) override;

    // IGpuResourceObserver
    void on_gpu_resource_destroyed(IGpuResource* resource) override;

    TextureId find_texture(ISurface* surf) const override;
    void register_texture(ISurface* surf, TextureId tid) override;
    void unregister_texture(ISurface* surf) override;
    TextureId ensure_texture_storage(ISurface* surf, const TextureDesc& desc) override;

    BufferEntry* find_buffer(IBuffer* buf) override;
    void register_buffer(IBuffer* buf, const BufferEntry& entry) override;
    void unregister_buffer(IBuffer* buf) override;
    BufferEntry* ensure_buffer_storage(IBuffer* buf, const GpuBufferDesc& desc) override;

    bool register_pipeline(IProgram* prog, PipelineId pid) override;

    void add_env_observer(const IBuffer::WeakPtr& res) override;

    void defer_texture_destroy(TextureId tid, uint64_t completion_marker) override;
    void defer_buffer_destroy(GpuBuffer handle, uint64_t completion_marker) override;
    void defer_pipeline_destroy(PipelineId pid, uint64_t completion_marker) override;

    void drain_deferred(IRenderBackend& backend) override;

    void on_resource_destroyed(IGpuResource* resource,
                               uint64_t completion_marker) override;

    void shutdown() override;

    size_t deferred_buffer_count() const override
    {
        std::lock_guard<std::mutex> lock(deferred_mutex_);
        return deferred_buffers_.size();
    }
    size_t deferred_texture_count() const override
    {
        std::lock_guard<std::mutex> lock(deferred_mutex_);
        return deferred_textures_.size();
    }
    size_t deferred_group_count() const override
    {
        std::lock_guard<std::mutex> lock(deferred_mutex_);
        return deferred_groups_.size();
    }

private:
    struct DeferredTextureDestroy
    {
        TextureId tid;
        uint64_t completion_marker;
    };
    struct DeferredBufferDestroy
    {
        GpuBuffer handle;
        uint64_t completion_marker;
    };
    struct DeferredPipelineDestroy
    {
        PipelineId pid;
        uint64_t completion_marker;
    };
    struct DeferredGroupDestroy
    {
        RenderTargetGroup handle;
        uint64_t completion_marker;
    };

    /// Number of consecutive `drain_deferred` ticks an idle transient
    /// pool entry may survive before falling through to deferred
    /// destroy. Active only when `transient_mode_` is set.
    static constexpr uint32_t kMaxIdleFrames = 8;

    /// Stored copy of `TextureGroupDesc` for the transient pool.
    /// The original `TextureGroupDesc::formats` is a non-owning view.
    struct StoredGroupDesc
    {
        int width;
        int height;
        DepthFormat depth;
        vector<PixelFormat> formats;
    };

    struct PooledTexture
    {
        TextureDesc desc;
        TextureId handle;
        uint64_t completion_marker;
        uint32_t idle_frames;
    };

    struct PooledGroup
    {
        StoredGroupDesc desc;
        RenderTargetGroup handle;
        uint64_t completion_marker;
        uint32_t idle_frames;
    };

    static bool transient_desc_matches(const TextureDesc& a, const TextureDesc& b);
    static bool transient_group_matches(const StoredGroupDesc& a, const TextureGroupDesc& b);
    static StoredGroupDesc store_group_desc(const TextureGroupDesc& d);

    /// Wraps an already-allocated `TextureId` in a fresh `IRenderTarget`
    /// shell, registers it for tracking, and subscribes the observer.
    /// Mirrors the tail of `create_render_texture` past backend allocation.
    IRenderTarget::Ptr wrap_pooled_texture(TextureId tid, const TextureDesc& desc);
    IRenderTextureGroup::Ptr wrap_pooled_group(RenderTargetGroup group,
                                               const StoredGroupDesc& desc);

    IRenderBackend* backend_ = nullptr;

    std::unordered_map<ISurface*, TextureId> texture_map_;
    std::unordered_map<ISurface*, RenderTargetGroup> group_map_;
    std::unordered_map<IBuffer*, BufferEntry> buffer_map_;
    std::unordered_map<IProgram*, PipelineId> pipeline_map_;
    vector<DeferredTextureDestroy> deferred_textures_;
    vector<DeferredGroupDestroy> deferred_groups_;
    vector<DeferredBufferDestroy> deferred_buffers_;
    vector<DeferredPipelineDestroy> deferred_pipelines_;
    mutable std::mutex deferred_mutex_;
    vector<IBuffer::WeakPtr> observed_env_resources_;

    /// Transient-pool state. Empty / inactive when `transient_mode_`
    /// is false (the default for the renderer's persistent manager).
    bool transient_mode_ = false;
    std::unordered_map<IGpuResource*, TextureDesc> transient_texture_descs_;
    std::unordered_map<IGpuResource*, StoredGroupDesc> transient_group_descs_;
    vector<PooledTexture> transient_pool_textures_;
    vector<PooledGroup>   transient_pool_groups_;
};

} // namespace velk

#endif // VELK_UI_GPU_RESOURCE_MANAGER_H
