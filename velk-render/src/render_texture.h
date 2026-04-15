#ifndef VELK_RENDER_RENDER_TEXTURE_H
#define VELK_RENDER_RENDER_TEXTURE_H

#include <velk-render/ext/gpu_resource.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/plugin.h>

namespace velk {

/**
 * @brief A texture that can be both rendered into and sampled.
 *
 * Implements IRenderTarget (for use as a begin_pass target) and ISurface
 * (for querying dimensions and format). Sampled in shaders via its
 * bindless TextureId (get_render_target_id).
 *
 * Created via instance().create<IObject>(ClassId::RenderTexture).
 * Set size and format before first use.
 */
class RenderTexture : public ext::GpuResource<RenderTexture, IRenderTarget>
{
public:
    VELK_CLASS_UID(ClassId::RenderTexture, "RenderTexture");

    GpuResourceType get_type() const override { return GpuResourceType::Texture; }

    // IRenderTarget
    uint64_t get_render_target_id() const override { return render_target_id_; }
    void set_render_target_id(uint64_t id) override { render_target_id_ = id; }

    // ISurface
    uvec2 get_dimensions() const override { return size_; }
    PixelFormat format() const override { return format_; }

    void set_size(uint32_t w, uint32_t h) { size_ = {w, h}; }
    void set_format(PixelFormat fmt) { format_ = fmt; }

private:
    uint64_t render_target_id_ = 0;
    uvec2 size_{};
    PixelFormat format_ = PixelFormat::RGBA8;
};

} // namespace velk

#endif // VELK_RENDER_RENDER_TEXTURE_H
