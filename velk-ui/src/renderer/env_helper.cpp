#include "env_helper.h"

#include <velk/api/state.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_surface.h>

#include <algorithm>

namespace velk::ui {

namespace {

int compute_mip_levels(int w, int h)
{
    int side = std::max(w, h);
    int levels = 1;
    while (side > 1) { side >>= 1; ++levels; }
    return levels;
}

} // namespace

EnvResolved ensure_env_ready(ICamera& camera, FrameContext& ctx)
{
    EnvResolved out{};
    if (!ctx.backend || !ctx.resources) {
        return out;
    }

    auto cam_state = read_state<ICamera>(&camera);
    if (!(cam_state && cam_state->environment)) {
        return out;
    }
    auto env_ptr = cam_state->environment.get<IEnvironment>();
    if (!env_ptr) {
        return out;
    }
    auto* surf = interface_cast<ISurface>(env_ptr);
    auto* buf = interface_cast<IBuffer>(env_ptr);
    if (!surf || !buf) {
        return out;
    }

    // Upload the environment texture if dirty. First-time path also creates
    // the bindless texture and wires the observer.
    if (buf->is_dirty()) {
        const uint8_t* pixels = buf->get_data();
        auto sz = surf->get_dimensions();
        int tw = static_cast<int>(sz.x);
        int th = static_cast<int>(sz.y);
        if (pixels && tw > 0 && th > 0) {
            TextureId tid = ctx.resources->find_texture(surf);
            if (tid == 0) {
                TextureDesc desc{};
                desc.width = tw;
                desc.height = th;
                desc.format = surf->format();
                // Mip chain approximates roughness prefilter: sampling
                // higher LODs blurs the reflection for rough surfaces.
                // Backend bilinear-downsamples at upload time; a proper
                // GGX prefilter is a future upgrade.
                desc.mip_levels = compute_mip_levels(tw, th);
                tid = ctx.backend->create_texture(desc);
                ctx.resources->register_texture(surf, tid);
                if (ctx.observer) {
                    surf->add_gpu_resource_observer(ctx.observer);
                    auto buf_ptr = interface_pointer_cast<IBuffer>(env_ptr);
                    if (buf_ptr) {
                        ctx.resources->add_env_observer(buf_ptr);
                    }
                }
            }
            if (tid != 0) {
                ctx.backend->upload_texture(tid, pixels, tw, th);
            }
            buf->clear_dirty();
        }
    }

    out.env = env_ptr.get();
    out.surface = surf;
    out.texture_id = ctx.resources->find_texture(surf);
    return out;
}

} // namespace velk::ui
