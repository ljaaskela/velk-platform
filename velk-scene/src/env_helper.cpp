#include "env_helper.h"

#include <velk/api/state.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_surface.h>

#include <algorithm>

namespace velk {

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
            const bool first_time = ctx.resources->find_texture(surf) == nullptr;
            TextureDesc desc{};
            desc.width = tw;
            desc.height = th;
            desc.format = surf->format();
            desc.sampler = surf->get_sampler_desc();
            // Mip chain approximates roughness prefilter: sampling
            // higher LODs blurs the reflection for rough surfaces.
            // Backend bilinear-downsamples at upload time; a proper
            // GGX prefilter is a future upgrade.
            desc.mip_levels = compute_mip_levels(tw, th);
            IGpuTexture* tex = ctx.resources->ensure_texture_storage(surf, desc);
            if (tex) {
                if (first_time) {
                    // Track for shutdown unsubscribe. The observer
                    // subscription itself is handled inside
                    // register_texture (called by ensure_texture_storage).
                    auto buf_ptr = interface_pointer_cast<IBuffer>(env_ptr);
                    if (buf_ptr) {
                        ctx.resources->add_env_observer(buf_ptr);
                    }
                }
                ctx.backend->upload_texture(*tex, pixels, tw, th);
            }
            buf->clear_dirty();
        }
    }

    out.env = env_ptr.get();
    out.surface = surf;
    if (auto* tex = ctx.resources->find_texture(surf)) {
        out.texture_id = get_texture_id(tex);
    }
    return out;
}

} // namespace velk
