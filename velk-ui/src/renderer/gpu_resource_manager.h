#ifndef VELK_UI_GPU_RESOURCE_MANAGER_H
#define VELK_UI_GPU_RESOURCE_MANAGER_H

#include <velk/vector.h>

#include <unordered_map>
#include <mutex>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_gpu_resource.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_surface.h>

namespace velk::ui {

/**
 * @brief Manages GPU resource upload tracking and deferred destruction.
 *
 * Tracks texture (ISurface) and buffer (IBuffer) mappings from CPU objects
 * to backend handles. Handles deferred destruction to ensure GPU-safe lifetimes.
 */
class GpuResourceManager
{
public:
    struct BufferEntry
    {
        GpuBuffer handle{};
        size_t size = 0;
    };

    /** @brief Returns the TextureId for a surface, or 0 if not tracked. */
    TextureId find_texture(ISurface* surf) const;

    /** @brief Registers a surface -> TextureId mapping. */
    void register_texture(ISurface* surf, TextureId tid);

    /** @brief Returns the buffer entry, or nullptr if not tracked. */
    BufferEntry* find_buffer(IBuffer* buf);

    /** @brief Registers a buffer entry. */
    void register_buffer(IBuffer* buf, const BufferEntry& entry);

    /** @brief Unregisters a buffer. */
    void unregister_buffer(IBuffer* buf);

    /** @brief Adds a weak reference to an environment resource for shutdown cleanup. */
    void add_env_observer(const IBuffer::WeakPtr& res);

    /** @brief Removes gpu resource observer from all tracked environment resources. */
    void unregister_env_observers(IGpuResourceObserver* observer);

    /** @brief Defers a texture for destruction after the safe window. */
    void defer_texture_destroy(TextureId tid, uint64_t safe_after);

    /** @brief Defers a buffer for destruction after the safe window. */
    void defer_buffer_destroy(GpuBuffer handle, uint64_t safe_after);

    /** @brief Drains deferred destruction queues for handles past the safe window. */
    void drain_deferred(IRenderBackend& backend, uint64_t present_counter);

    /** @brief Called when a GPU resource is destroyed. Enqueues handles for deferred destruction. */
    void on_resource_destroyed(IGpuResource* resource, uint64_t present_counter, uint64_t latency_frames);

    /** @brief Destroys all tracked resources during shutdown. */
    void shutdown(IRenderBackend& backend);

private:
    struct DeferredTextureDestroy
    {
        TextureId tid;
        uint64_t safe_after_frame;
    };
    struct DeferredBufferDestroy
    {
        GpuBuffer handle;
        uint64_t safe_after_frame;
    };

    std::unordered_map<ISurface*, TextureId> texture_map_;
    std::unordered_map<IBuffer*, BufferEntry> buffer_map_;
    vector<DeferredTextureDestroy> deferred_textures_;
    vector<DeferredBufferDestroy> deferred_buffers_;
    std::mutex deferred_mutex_;
    vector<IBuffer::WeakPtr> observed_env_resources_;
};

} // namespace velk::ui

#endif // VELK_UI_GPU_RESOURCE_MANAGER_H
