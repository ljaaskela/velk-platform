#include "path/post_process.h"

#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/interface/intf_interface.h>
#include <velk/interface/intf_object_storage.h>

#include <velk-render/interface/intf_gpu_resource.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_pass.h>
#include <velk-render/interface/intf_surface.h>
#include <velk-render/interface/intf_window_surface.h>
#include <velk-render/plugin.h>

namespace velk::impl {

namespace {

/// Discover effect children attached to the given storage.
::velk::vector<::velk::IEffect::Ptr> resolve_effects(::velk::IInterface* self)
{
    ::velk::vector<::velk::IEffect::Ptr> effects;
    auto* storage = interface_cast<::velk::IObjectStorage>(self);
    if (!storage) return effects;
    ::velk::AttachmentQuery q;
    q.interfaceUid = ::velk::IEffect::UID;
    for (auto& a : storage->find_attachments(q)) {
        if (auto p = interface_pointer_cast<::velk::IEffect>(a)) {
            effects.emplace_back(std::move(p));
        }
    }
    return effects;
}

} // namespace

void PostProcess::emit(::velk::ViewEntry& view,
                      ::velk::IRenderTarget::Ptr input,
                      ::velk::IRenderTarget::Ptr output,
                      ::velk::FrameContext& ctx,
                      ::velk::IRenderGraph& graph)
{
    auto effects = resolve_effects(static_cast<::velk::IPostProcess*>(this));

    // Determine intermediate dimensions. For tier 1 we mirror the
    // input target's size; if input has no usable size we fall back
    // to view.surface dimensions.
    int w = 0;
    int h = 0;
    if (auto* surf = interface_cast<::velk::ISurface>(input.get())) {
        auto dims = surf->get_dimensions();
        w = static_cast<int>(dims.x);
        h = static_cast<int>(dims.y);
    }
    if ((w <= 0 || h <= 0) && view.surface) {
        if (auto state = ::velk::read_state<::velk::IWindowSurface>(view.surface)) {
            w = state->size.x;
            h = state->size.y;
        }
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    /// Each non-last effect writes to a container-allocated storage
    /// intermediate (RGBA8). The last effect writes directly to the
    /// caller-supplied `output`. The container's caller guarantees
    /// `output` is a storage-writable texture so effects don't
    /// special-case "am I the last stage?". An empty container does a
    /// passthrough blit so the path output still reaches `output`.
    if (effects.empty()) {
        auto passthrough = ::velk::instance().create<::velk::IRenderPass>(
            ::velk::ClassId::DefaultRenderPass);
        if (!passthrough) return;
        passthrough->add_op(::velk::ops::BlitToSurface{
            static_cast<::velk::TextureId>(
                input->get_gpu_handle(::velk::GpuResourceKey::Default)),
            output->get_gpu_handle(::velk::GpuResourceKey::Default),
            {0, 0, static_cast<float>(w), static_cast<float>(h)}});
        passthrough->add_read(interface_pointer_cast<::velk::IGpuResource>(input));
        passthrough->add_write(interface_pointer_cast<::velk::IGpuResource>(output));
        graph.add_pass(std::move(passthrough));
        return;
    }

    auto current = input;
    for (size_t i = 0; i + 1 < effects.size(); ++i) {
        auto next = ensure_intermediate(view, i, w, h, ctx, graph);
        if (!next) return;
        effects[i]->emit(view, current, next, ctx, graph);
        current = next;
    }
    effects.back()->emit(view, current, output, ctx, graph);
}

::velk::IRenderTarget::Ptr
PostProcess::ensure_intermediate(::velk::ViewEntry& view,
                                 size_t index,
                                 int width, int height,
                                 ::velk::FrameContext& /*ctx*/,
                                 ::velk::IRenderGraph& graph)
{
    auto& vs = view_states_[&view];

    if (index >= vs.intermediates.size()) {
        vs.intermediates.resize(index + 1);
    }

    ::velk::TextureDesc td{};
    td.width = width;
    td.height = height;
    td.format = ::velk::PixelFormat::RGBA8;
    td.usage = ::velk::TextureUsage::Storage;
    // Drop the prior frame's Ptr (manager parks the handle on its
    // pool) and request a fresh one. On a steady-state hit the pool
    // re-issues the parked handle wrapped in a new shell.
    vs.intermediates[index] = graph.resources().create_render_texture(td);
    return vs.intermediates[index];
}

void PostProcess::release_view_state(ViewState& /*vs*/, ::velk::FrameContext& /*ctx*/)
{
    // intermediates are managed: dropping the Ptrs (when the
    // ViewState is erased / intermediates are cleared) auto-defers
    // the backend handles via the resource manager observer chain.
}

void PostProcess::on_view_removed(::velk::ViewEntry& view,
                                  ::velk::FrameContext& ctx)
{
    auto it = view_states_.find(&view);
    if (it != view_states_.end()) {
        release_view_state(it->second, ctx);
        view_states_.erase(it);
    }

    // Forward to children so view-keyed effects can release too.
    for (auto& effect : resolve_effects(static_cast<::velk::IPostProcess*>(this))) {
        effect->on_view_removed(view, ctx);
    }
}

void PostProcess::shutdown(::velk::FrameContext& ctx)
{
    for (auto& [v, vs] : view_states_) {
        release_view_state(vs, ctx);
    }
    view_states_.clear();

    for (auto& effect : resolve_effects(static_cast<::velk::IPostProcess*>(this))) {
        effect->shutdown(ctx);
    }
}

} // namespace velk::impl
