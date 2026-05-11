#ifndef VELK_RENDER_RENDER_TEXTURE_H
#define VELK_RENDER_RENDER_TEXTURE_H

#include <velk-render/ext/gpu_resource.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/plugin.h>

namespace velk {

/**
 * @brief A texture that can be both rendered into and sampled.
 *
 * Implements IRenderTarget (renderable) and ISurface (queryable
 * dimensions and format). Sampled in shaders via its bindless
 * TextureId (get_render_target_id).
 *
 * Created via instance().create<IObject>(ClassId::RenderTexture).
 * Set size and format before first use.
 */
class RenderTexture : public ext::GpuResource<RenderTexture, IRenderTarget>
{
public:
    VELK_CLASS_UID(ClassId::RenderTexture, "RenderTexture");

    GpuResourceType get_type() const override { return GpuResourceType::Texture; }

    // ISurface
    uvec2 get_dimensions() const override { return size_; }
    PixelFormat format() const override { return format_; }

    void set_size(uint32_t w, uint32_t h) override { size_ = {w, h}; }
    void set_format(PixelFormat fmt) override { format_ = fmt; }

    DepthFormat get_depth_format() const override { return depth_format_; }
    void set_depth_format(DepthFormat df) override { depth_format_ = df; }

private:
    uvec2 size_{};
    PixelFormat format_ = PixelFormat::RGBA8;
    DepthFormat depth_format_ = DepthFormat::None;
};

} // namespace velk

#endif // VELK_RENDER_RENDER_TEXTURE_H
