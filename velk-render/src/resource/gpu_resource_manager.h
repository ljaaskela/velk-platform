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
        if (auto* gt = find_texture(surf)) {
            return get_texture_id(gt);
        }
        uint64_t rt_id = ::velk::get_render_target_id(surf);
        if (rt_id != 0) return static_cast<TextureId>(rt_id);
        return 0;
    }

    // IGpuResourceManager
    void init(IRenderBackend* backend) override;
    void enable_transient_pool() override;
    IGpuBuffer::Ptr create_gpu_buffer(const GpuBufferDesc& desc) override;
    IRenderTarget::Ptr create_render_texture(const TextureDesc& desc) override;
    IRenderTextureGroup::Ptr create_render_texture_group(
        const TextureGroupDesc& desc) override;

    // IGpuResourceObserver
    void on_gpu_resource_destroyed(IGpuResource* resource) override;

    IGpuTexture* find_texture(ISurface* surf) const override;
    void register_texture(ISurface* surf, IGpuTexture::Ptr tex) override;
    void unregister_texture(ISurface* surf) override;
    IGpuTexture* ensure_texture_storage(ISurface* surf, const TextureDesc& desc) override;

    BufferEntry* find_buffer(IBuffer* buf) override;
    void register_buffer(IBuffer* buf, const BufferEntry& entry) override;
    void unregister_buffer(IBuffer* buf) override;
    BufferEntry* ensure_buffer_storage(IBuffer* buf, const GpuBufferDesc& desc) override;

    void add_env_observer(const IBuffer::WeakPtr& res) override;

    void drain_deferred(IRenderBackend& backend) override;

    void on_resource_destroyed(IGpuResource* resource,
                               uint64_t completion_marker) override;

    void shutdown() override;

    size_t deferred_texture_count() const override { return 0; }
    size_t deferred_group_count() const override { return 0; }

private:

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
        IGpuTexture::Ptr handle;
        uint64_t completion_marker;
        uint32_t idle_frames;
    };

    struct PooledGroup
    {
        StoredGroupDesc desc;
        IRenderTextureGroup::Ptr handle;
        uint64_t completion_marker;
        uint32_t idle_frames;
    };

    static bool transient_desc_matches(const TextureDesc& a, const TextureDesc& b);
    static bool transient_group_matches(const StoredGroupDesc& a, const TextureGroupDesc& b);
    static StoredGroupDesc store_group_desc(const TextureGroupDesc& d);

    /// Wraps an already-allocated `IGpuTexture::Ptr` in a fresh
    /// `IRenderTarget` shell, registers it for tracking, and subscribes
    /// the observer. Mirrors the tail of `create_render_texture` past
    /// backend allocation.
    IRenderTarget::Ptr wrap_pooled_texture(IGpuTexture::Ptr tex, const TextureDesc& desc);
    IRenderTextureGroup::Ptr wrap_pooled_group(IRenderTextureGroup::Ptr group,
                                               const StoredGroupDesc& desc);

    IRenderBackend* backend_ = nullptr;

    std::unordered_map<ISurface*, IGpuTexture::Ptr> texture_map_;
    std::unordered_map<IBuffer*, BufferEntry> buffer_map_;
    /// Live IGpuBuffers handed out by `create_gpu_buffer`. Raw
    /// pointers; populated on creation, removed by the observer
    /// callback on destruction. Owning Ptrs live with the callers.
    std::unordered_map<IGpuResource*, IGpuBuffer*> tracked_gpu_buffers_;
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
