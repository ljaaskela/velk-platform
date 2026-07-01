#include "gpu_resource_manager.h"

#include <velk/api/velk.h>
#include <velk-render/interface/intf_gpu_buffer.h>
#include <velk-render/plugin.h>

namespace velk {

GpuResourceManager::~GpuResourceManager()
{
    GpuResourceManager::shutdown();
}

void GpuResourceManager::init(IRenderBackend* backend)
{
    backend_ = backend;
}

void GpuResourceManager::enable_transient_pool()
{
    transient_mode_ = true;
}

IGpuBuffer::Ptr GpuResourceManager::create_gpu_buffer(const GpuBufferDesc& desc)
{
    if (!backend_) return {};
    auto gb = backend_->create_gpu_buffer(desc);
    if (!gb) return {};
    tracked_gpu_buffers_[gb.get()] = gb.get();
    gb->add_gpu_resource_observer(this);
    return gb;
}

IGpuArena::Ptr GpuResourceManager::create_arena(uint32_t slot, uint32_t element_size)
{
    auto arena = ::velk::instance().create<IGpuArena>(ClassId::GpuArena);
    if (!arena) return {};
    arena->init(slot, element_size);
    arenas_.push_back(arena);  // weak-track for the reclaim tick
    return arena;
}

bool GpuResourceManager::transient_desc_matches(const TextureDesc& a, const TextureDesc& b)
{
    return a.width == b.width && a.height == b.height &&
           a.mip_levels == b.mip_levels && a.format == b.format &&
           a.usage == b.usage && a.sampler == b.sampler;
}

bool GpuResourceManager::transient_group_matches(const StoredGroupDesc& a,
                                                 const TextureGroupDesc& b)
{
    if (a.width != b.width || a.height != b.height || a.depth != b.depth) return false;
    if (a.formats.size() != b.formats.size()) return false;
    for (uint32_t i = 0; i < a.formats.size(); ++i) {
        if (a.formats[i] != b.formats[i]) return false;
    }
    return true;
}

GpuResourceManager::StoredGroupDesc
GpuResourceManager::store_group_desc(const TextureGroupDesc& d)
{
    StoredGroupDesc s;
    s.width = d.width;
    s.height = d.height;
    s.depth = d.depth;
    s.formats.reserve(static_cast<uint32_t>(d.formats.size()));
    for (uint32_t i = 0; i < d.formats.size(); ++i) {
        s.formats.push_back(d.formats[i]);
    }
    return s;
}

IRenderTarget::Ptr
GpuResourceManager::wrap_pooled_texture(IGpuTexture::Ptr tex, const TextureDesc& desc)
{
    auto rt = instance().create<IRenderTarget>(ClassId::RenderTexture);
    if (!rt) return {};
    rt->set_size(desc.width, desc.height);
    rt->set_format(desc.format);
    register_texture(rt.get(), std::move(tex));
    rt->add_gpu_resource_observer(this);
    transient_texture_descs_[rt.get()] = desc;
    return rt;
}

IRenderTextureGroup::Ptr
GpuResourceManager::wrap_pooled_group(IRenderTextureGroup::Ptr /*group*/,
                                      const StoredGroupDesc& /*desc*/)
{
    // Transient pool for groups was retired in Slice B; the underlying
    // wrapper now lives behind an IRenderTextureGroup::Ptr whose dtor
    // auto-defers, so we can't park it for reuse via the observer
    // pattern (the wrapper destruction is the trigger). Returning
    // empty causes the create path to allocate fresh.
    return {};
}

void GpuResourceManager::on_gpu_resource_destroyed(IGpuResource* resource)
{
    on_resource_destroyed(
        resource, backend_ ? backend_->pending_frame_completion_marker() : 0);
}

IRenderTarget::Ptr GpuResourceManager::create_render_texture(const TextureDesc& desc)
{
    if (!backend_) return {};

    // Transient-pool fast path: scan the free-list for a matching
    // description whose GPU work has finished, reuse its handle.
    if (transient_mode_) {
        for (auto it = transient_pool_textures_.begin();
             it != transient_pool_textures_.end(); ++it) {
            if (!transient_desc_matches(it->desc, desc)) continue;
            if (!backend_->is_frame_complete(it->completion_marker)) continue;
            IGpuTexture::Ptr tex = std::move(it->handle);
            transient_pool_textures_.erase(it);
            if (auto rt = wrap_pooled_texture(std::move(tex), desc)) return rt;
            // Wrap failure: dropping `tex` defers via ~VkRenderTexture.
            break;
        }
    }

    auto tex = backend_->create_texture(desc);
    if (!tex) return {};

    auto rt = instance().create<IRenderTarget>(ClassId::RenderTexture);
    if (!rt) {
        // Dropping `tex` here defers via ~VkRenderTexture.
        return {};
    }
    rt->set_size(desc.width, desc.height);
    rt->set_format(desc.format);

    // Registers the IGpuTexture::Ptr in texture_map_ keyed by ISurface*
    // (the rt itself) AND stamps `gpu_handle(Default) = bindless` on the
    // wrapper.
    register_texture(rt.get(), std::move(tex));

    // Subscribe the renderer's observer so the rt's dtor triggers
    // on_resource_destroyed, which drops the IGpuTexture::Ptr (and
    // ~VkRenderTexture defers via the backend marker queue) or parks
    // it on the transient pool.
    rt->add_gpu_resource_observer(this);

    if (transient_mode_) {
        transient_texture_descs_[rt.get()] = desc;
    }
    return rt;
}

IRenderTextureGroup::Ptr GpuResourceManager::create_render_texture_group(
    const TextureGroupDesc& desc)
{
    if (!backend_ || desc.formats.empty() || desc.width <= 0 || desc.height <= 0) return {};

    auto rtg = backend_->create_render_target_group(desc);
    if (!rtg) {
        return {};
    }

    // Track for lifecycle: on_resource_destroyed looks up the group
    // Lifecycle: the user's IRenderTextureGroup::Ptr is the only strong
    // ref; ~VkRenderTargetGroup defers backend handles for destroy. No
    // manager-side observer needed.
    return rtg;
}

IGpuTexture* GpuResourceManager::find_texture(ISurface* surf) const
{
    auto it = texture_map_.find(surf);
    return it != texture_map_.end() ? it->second.get() : nullptr;
}

void GpuResourceManager::register_texture(ISurface* surf, IGpuTexture::Ptr tex)
{
    if (!surf) return;
    const TextureId tid = get_texture_id(tex);
    surf->set_gpu_handle(GpuResourceKey::Default, static_cast<uint64_t>(tid));
    texture_map_[surf] = std::move(tex);
    // Subscribe so dropping the wrapper auto-drops the texture entry.
    // Idempotent: re-registration on resize doesn't double-subscribe.
    surf->add_gpu_resource_observer(this);
}

void GpuResourceManager::unregister_texture(ISurface* surf)
{
    texture_map_.erase(surf);
}

IGpuTexture* GpuResourceManager::ensure_texture_storage(ISurface* surf, const TextureDesc& desc)
{
    if (!surf || !backend_) return nullptr;
    if (auto* existing = find_texture(surf)) return existing;
    auto tex = backend_->create_texture(desc);
    if (!tex) return nullptr;
    IGpuTexture* raw = tex.get();
    register_texture(surf, std::move(tex));
    return raw;
}

IGpuResourceManager::BufferEntry* GpuResourceManager::find_buffer(IBuffer* buf)
{
    auto it = buffer_map_.find(buf);
    return it != buffer_map_.end() ? &it->second : nullptr;
}

void GpuResourceManager::register_buffer(IBuffer* buf, const BufferEntry& entry)
{
    if (!buf) return;
    buffer_map_[buf] = entry;
    // Subscribe so dropping the wrapper drops our entry too. IBuffer
    // doesn't extend IGpuResource directly; concrete impls implement
    // it via IGpuBuffer or ISurface (or both).
    if (auto* gr = interface_cast<IGpuResource>(buf)) {
        gr->add_gpu_resource_observer(this);
    }
}

void GpuResourceManager::unregister_buffer(IBuffer* buf)
{
    buffer_map_.erase(buf);
}

IGpuResourceManager::BufferEntry*
GpuResourceManager::ensure_buffer_storage(IBuffer* buf, const GpuBufferDesc& desc)
{
    if (!buf || !backend_) return nullptr;
    auto* owner = interface_cast<IGpuBufferStorageOwner>(buf);
    if (!owner) return nullptr;        // pure CPU IBuffer; no GPU storage

    auto* be = find_buffer(buf);
    if (be && be->size != desc.size) {
        owner->attach_gpu_buffer({});
        unregister_buffer(buf);
        be = nullptr;
    }
    if (!be) {
        IGpuBuffer::Ptr gb = create_gpu_buffer(desc);
        if (!gb) return nullptr;

        BufferEntry entry{};
        entry.buffer = gb;
        entry.size = desc.size;
        register_buffer(buf, entry);
        owner->attach_gpu_buffer(std::move(gb));

        be = find_buffer(buf);
    }
    return be;
}

void GpuResourceManager::add_env_observer(const IBuffer::WeakPtr& res)
{
    observed_env_resources_.push_back(res);
}

void GpuResourceManager::drain_deferred(IRenderBackend& /*backend*/)
{
    std::lock_guard<std::mutex> lock(deferred_mutex_);

    // Reclaim freed arena regions whose in-flight frame has retired, and
    // prune arenas the caller has dropped.
    for (size_t i = 0; i < arenas_.size();) {
        if (auto a = arenas_[i].lock()) {
            a->reclaim();
            ++i;
        } else {
            arenas_[i] = arenas_.back();
            arenas_.pop_back();
        }
    }

    // Transient-pool tick: age idle texture entries; drop the Ptr after
    // `kMaxIdleFrames` consecutive idle ticks, which routes through
    // ~VkRenderTexture -> backend defer queue. Groups no longer pool
    // (Slice B); their lifecycle is direct via Ptr drop.
    if (transient_mode_) {
        for (auto it = transient_pool_textures_.begin();
             it != transient_pool_textures_.end();) {
            it->idle_frames++;
            if (it->idle_frames >= kMaxIdleFrames) {
                it = transient_pool_textures_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void GpuResourceManager::on_resource_destroyed(IGpuResource* resource,
                                               uint64_t completion_marker)
{
    // Transient-pool intercept: park the backend handle on the pool
    // free-list keyed by description, instead of immediately enqueueing
    // for deferred destroy. A subsequent matching `create_*` reuses it
    // once `is_frame_complete(completion_marker)` resolves.
    if (transient_mode_) {
        {
            auto td = transient_texture_descs_.find(resource);
            if (td != transient_texture_descs_.end()) {
                TextureDesc desc = td->second;
                transient_texture_descs_.erase(td);

                std::lock_guard<std::mutex> lock(deferred_mutex_);
                if (auto* surf = interface_cast<ISurface>(resource)) {
                    auto it = texture_map_.find(surf);
                    if (it != texture_map_.end()) {
                        transient_pool_textures_.push_back(
                            {desc, std::move(it->second), completion_marker, 0});
                        texture_map_.erase(it);
                        return;
                    }
                }
            }
        }
        // Fall through to base behaviour for untracked resources.
    }

    std::lock_guard<std::mutex> lock(deferred_mutex_);

    if (auto* surf = interface_cast<ISurface>(resource)) {
        auto it = texture_map_.find(surf);
        if (it != texture_map_.end()) {
            // Dropping the Ptr triggers ~VkRenderTexture which defers
            // backend handles via the backend's marker queue.
            texture_map_.erase(it);
            return;
        }
    }

    if (auto* buf = interface_cast<IBuffer>(resource)) {
        buffer_map_.erase(buf);
    }

    // IGpuBuffer destruction is driven by ~VkGpuBuffer (which defers
    // VMA destroy via the backend marker queue while the derived
    // vtable is still intact). The manager just drops its tracking
    // entry — calling virtual methods on @p resource here would hit
    // pure-virtual stubs since the derived dtor body has already
    // run by the time the observer chain notifies.
    tracked_gpu_buffers_.erase(resource);

    (void)completion_marker;
}

void GpuResourceManager::shutdown()
{
    if (!backend_) return;

    auto unregister = [](IGpuResource* res, IGpuResourceObserver* obs) {
        if (res) {
            res->remove_gpu_resource_observer(obs);
        }
    };

    // Detach from any env resources we still observe so their CPU
    // dtors (which may run later) don't reach a dead manager.
    for (auto& weak : observed_env_resources_) {
        if (auto key = weak.lock()) {
            unregister(interface_cast<IGpuResource>(key.get()), this);
        }
    }
    observed_env_resources_.clear();

    {
        std::lock_guard<std::mutex> lock(deferred_mutex_);

        // Drop transient-pool texture Ptrs (~VkRenderTexture defers via
        // the backend marker queue).
        transient_pool_textures_.clear();
        transient_texture_descs_.clear();
    }

    // Dropping each Ptr triggers ~VkRenderTexture which defers backend
    // handles. The renderer's `wait_idle` call before us guarantees
    // those defers can be drained immediately afterwards.
    for (auto& [key, _] : texture_map_) {
        unregister(key, this);
    }
    texture_map_.clear();

    // Detach GPU storage from each tracked IBuffer while the backend
    // is still alive. Plugin-held IBuffers (e.g. Font's glyph buffers
    // in the text plugin singleton) outlive the manager; without this
    // their inner IGpuBuffer Ptr would only drop later, past
    // ~VkBackend.
    for (auto& [key, entry] : buffer_map_) {
        unregister(interface_cast<IGpuResource>(key), this);
        if (auto* owner = interface_cast<IGpuBufferStorageOwner>(key)) {
            owner->attach_gpu_buffer({});
        }
    }
    buffer_map_.clear();

    // Late ~VkGpuBuffer calls then take the unmanaged path and defer
    // themselves; deferring here would double-free.
    for (auto& [res, gb] : tracked_gpu_buffers_) {
        unregister(res, this);
    }
    tracked_gpu_buffers_.clear();
}

} // namespace velk
