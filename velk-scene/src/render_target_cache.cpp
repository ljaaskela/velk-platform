#include "render_target_cache.h"

#include "batch_builder.h"

#include <velk/api/state.h>
#include <velk/interface/intf_object_storage.h>

#include <velk-render/detail/intf_gpu_resource_manager_internal.h>
#include <velk-render/frame/render_view.h>
#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-scene/interface/intf_render_to_texture.h>

#include <velk/api/velk.h>

#include <algorithm>
#include <cstring>

namespace velk {

namespace {

void build_ortho_projection(float* out, float width, float height)
{
    std::memset(out, 0, 16 * sizeof(float));
    out[0] = 2.0f / width;
    out[5] = 2.0f / height;
    out[10] = -1.0f;
    out[12] = -1.0f;
    out[13] = -1.0f;
    out[15] = 1.0f;
}

} // namespace

void RenderTargetCache::ensure(FrameContext& ctx, BatchBuilder& batch_builder)
{
    if (!ctx.backend || !ctx.resources) {
        return;
    }
    for (auto& rtp : batch_builder.render_target_passes()) {
        auto rtt = ::velk::find_attachment<IRenderToTexture>(rtp.element);
        if (!rtt) continue;

        // The user supplies the RenderTexture wrapper via the trait
        // state. It's a named identity wired declaratively (e.g. JSON
        // ties producer + consumer to the same instance); the cache
        // resolves its backend handle lazily.
        auto state = read_state<IRenderToTexture>(rtt);
        if (!state) continue;
        auto target = state->render_target.get<IRenderTarget>();
        if (!target) continue;

        int w{1}, h{1};
        if (auto es = read_state<IElement>(rtp.element)) {
            w = std::max(static_cast<int>(es->size.width), 1);
            h = std::max(static_cast<int>(es->size.height), 1);
        }
        if (state->texture_size.x > 0 && state->texture_size.y > 0) {
            w = static_cast<int>(state->texture_size.x);
            h = static_cast<int>(state->texture_size.y);
        }
        PixelFormat fmt = target->format();

        auto& rte = entries_[rtp.element];
        TextureId existing = static_cast<TextureId>(
            target->get_gpu_handle(GpuResourceKey::Default));

        // Resize / format-change: defer-destroy the old handle and
        // realloc into the same wrapper. The wrapper's lifetime stays
        // owned by the user; only the backend handle cycles.
        if (existing != 0 &&
            (rte.width != w || rte.height != h || rte.format != fmt)) {
            defer_texture_destroy(ctx.resources, existing, ctx.defer_marker);
            target->set_gpu_handle(GpuResourceKey::Default, 0);
            ctx.resources->unregister_texture(target.get());
            existing = 0;
        }

        if (existing == 0) {
            TextureDesc tdesc{};
            tdesc.width = w;
            tdesc.height = h;
            tdesc.format = fmt;
            tdesc.usage = TextureUsage::RenderTarget;
            TextureId tid = ctx.resources->ensure_texture_storage(target.get(), tdesc);
            if (tid == 0) continue;
            target->set_size(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
            // Observer subscription is handled inside register_texture
            // (called by ensure_texture_storage); when the user's last
            // reference drops, the manager auto-defers the backend handle.
            rte.target = target;
            rte.width = w;
            rte.height = h;
            rte.format = fmt;
        }
    }
}

void RenderTargetCache::emit_passes(FrameContext& ctx, BatchBuilder& batch_builder,
                                    IRenderGraph& graph)
{
    if (!ctx.backend || !ctx.frame_buffer || !ctx.pipeline_map) {
        return;
    }

    if (!forward_path_) {
        forward_path_ = instance().create<IRenderPath>(ClassId::Path::Forward);
        if (!forward_path_) return;
    }

    // RTT subtrees render in the format declared by their RenderTexture
    // (Surface by default, but RGBA16F etc. when the user wants HDR).
    // Stash + restore the FrameContext's format so per-view state set
    // during the camera loop isn't perturbed.
    PixelFormat saved_format = ctx.target_format;

    for (auto& rtp : batch_builder.render_target_passes()) {
        auto it = entries_.find(rtp.element);
        if (it == entries_.end()) continue;
        auto& rte = it->second;
        auto target = rte.target.lock();
        if (!target) continue;

        ctx.target_format = rte.format;

        FrameGlobals rt_globals{};
        build_ortho_projection(
            rt_globals.view_projection, static_cast<float>(rte.width), static_cast<float>(rte.height));
        rt_globals.viewport[0] = static_cast<float>(rte.width);
        rt_globals.viewport[1] = static_cast<float>(rte.height);
        rt_globals.viewport[2] = 1.0f / static_cast<float>(rte.width);
        rt_globals.viewport[3] = 1.0f / static_cast<float>(rte.height);
        rt_globals.bvh_root = ctx.bvh_root;
        rt_globals.bvh_node_count = ctx.bvh_node_count;
        rt_globals.bvh_shape_count = ctx.bvh_shape_count;
        rt_globals.bvh_nodes_addr = ctx.bvh_nodes_addr;
        rt_globals.bvh_shapes_addr = ctx.bvh_shapes_addr;
        rt_globals.present_counter = static_cast<uint32_t>(ctx.present_counter);
        RenderView rt_view{};
        rt_view.batches = &rtp.batches;
        rt_view.viewport = {0, 0,
                            static_cast<float>(rte.width),
                            static_cast<float>(rte.height)};
        rt_view.width = rte.width;
        rt_view.height = rte.height;
        rt_view.view_globals_address =
            ctx.frame_buffer->write(&rt_globals, sizeof(rt_globals));
        rt_view.bvh_root = ctx.bvh_root;
        rt_view.bvh_node_count = ctx.bvh_node_count;
        rt_view.bvh_shape_count = ctx.bvh_shape_count;
        rt_view.bvh_nodes_addr = ctx.bvh_nodes_addr;
        rt_view.bvh_shapes_addr = ctx.bvh_shapes_addr;

        auto& entry = view_entries_[rtp.element];
        entry.cached_width = rte.width;
        entry.cached_height = rte.height;

        forward_path_->build_passes(entry, rt_view, target, ctx, graph);
        rte.dirty = false;
    }

    ctx.target_format = saved_format;
}

void RenderTargetCache::on_element_removed(IElement* elem, FrameContext& /*ctx*/)
{
    // Erase the entry; the cached Ptr drops, resource manager
    // auto-defers the backend handle.
    entries_.erase(elem);
}

void RenderTargetCache::shutdown(FrameContext& /*ctx*/)
{
    entries_.clear();
}

} // namespace velk
