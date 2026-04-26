#include "gpu_resource_manager.h"

#include <velk/api/velk.h>

namespace velk {

TextureId GpuResourceManager::find_texture(ISurface* surf) const
{
    auto it = texture_map_.find(surf);
    return it != texture_map_.end() ? it->second : 0;
}

void GpuResourceManager::register_texture(ISurface* surf, TextureId tid)
{
    texture_map_[surf] = tid;
}

GpuResourceManager::BufferEntry* GpuResourceManager::find_buffer(IBuffer* buf)
{
    auto it = buffer_map_.find(buf);
    return it != buffer_map_.end() ? &it->second : nullptr;
}

void GpuResourceManager::register_buffer(IBuffer* buf, const BufferEntry& entry)
{
    buffer_map_[buf] = entry;
}

void GpuResourceManager::unregister_buffer(IBuffer* buf)
{
    buffer_map_.erase(buf);
}

bool GpuResourceManager::register_pipeline(IProgram* prog, PipelineId pid)
{
    if (!prog || !pid) {
        return false;
    }
    auto [it, inserted] = pipeline_map_.emplace(prog, pid);
    return inserted;
}

void GpuResourceManager::add_env_observer(const IBuffer::WeakPtr& res)
{
    observed_env_resources_.push_back(res);
}

void GpuResourceManager::unregister_env_observers(IGpuResourceObserver* observer)
{
    for (auto& weak : observed_env_resources_) {
        if (auto res = weak.lock()) {
            res->remove_gpu_resource_observer(observer);
        }
    }
    observed_env_resources_.clear();
}

void GpuResourceManager::defer_texture_destroy(TextureId tid, uint64_t safe_after)
{
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    deferred_textures_.push_back({tid, safe_after});
}

void GpuResourceManager::defer_buffer_destroy(GpuBuffer handle, uint64_t safe_after)
{
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    deferred_buffers_.push_back({handle, safe_after});
}

void GpuResourceManager::defer_pipeline_destroy(PipelineId pid, uint64_t safe_after)
{
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    deferred_pipelines_.push_back({pid, safe_after});
}

void GpuResourceManager::drain_deferred(IRenderBackend& backend, uint64_t present_counter)
{
    std::lock_guard<std::mutex> lock(deferred_mutex_);

    for (auto it = deferred_textures_.begin(); it != deferred_textures_.end();) {
        if (present_counter > it->safe_after_frame) {
            backend.destroy_texture(it->tid);
            it = deferred_textures_.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = deferred_buffers_.begin(); it != deferred_buffers_.end();) {
        if (present_counter > it->safe_after_frame) {
            backend.destroy_buffer(it->handle);
            it = deferred_buffers_.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = deferred_pipelines_.begin(); it != deferred_pipelines_.end();) {
        if (present_counter > it->safe_after_frame) {
            backend.destroy_pipeline(it->pid);
            it = deferred_pipelines_.erase(it);
        } else {
            ++it;
        }
    }
}

void GpuResourceManager::on_resource_destroyed(IGpuResource* resource, uint64_t present_counter,
                                            uint64_t latency_frames)
{
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    uint64_t safe_after = present_counter + latency_frames;

    if (auto* surf = interface_cast<ISurface>(resource)) {
        auto it = texture_map_.find(surf);
        if (it != texture_map_.end()) {
            deferred_textures_.push_back({it->second, safe_after});
            texture_map_.erase(it);
            return;
        }
    }

    if (auto* buf = interface_cast<IBuffer>(resource)) {
        auto it = buffer_map_.find(buf);
        if (it != buffer_map_.end()) {
            deferred_buffers_.push_back({it->second.handle, safe_after});
            buffer_map_.erase(it);
        }
    }

    if (auto* prog = interface_cast<IProgram>(resource)) {
        auto it = pipeline_map_.find(prog);
        if (it != pipeline_map_.end()) {
            deferred_pipelines_.push_back({it->second, safe_after});
            pipeline_map_.erase(it);
        }
    }
}

void GpuResourceManager::shutdown(IRenderBackend& backend)
{
    {
        std::lock_guard<std::mutex> lock(deferred_mutex_);
        for (auto& d : deferred_textures_) {
            backend.destroy_texture(d.tid);
        }
        deferred_textures_.clear();
        for (auto& d : deferred_buffers_) {
            backend.destroy_buffer(d.handle);
        }
        deferred_buffers_.clear();
        for (auto& d : deferred_pipelines_) {
            backend.destroy_pipeline(d.pid);
        }
        deferred_pipelines_.clear();
    }

    for (auto& [key, tid] : texture_map_) {
        backend.destroy_texture(tid);
    }
    texture_map_.clear();

    for (auto& [key, entry] : buffer_map_) {
        backend.destroy_buffer(entry.handle);
    }
    buffer_map_.clear();

    for (auto& [key, pid] : pipeline_map_) {
        backend.destroy_pipeline(pid);
    }
    pipeline_map_.clear();
}

} // namespace velk
