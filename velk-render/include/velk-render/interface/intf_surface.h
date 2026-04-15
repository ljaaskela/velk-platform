#ifndef VELK_RENDER_INTF_SURFACE_H
#define VELK_RENDER_INTF_SURFACE_H

#include <velk/api/math_types.h>
#include <velk/interface/intf_metadata.h>

#include <velk-render/interface/intf_gpu_resource.h>
#include <velk-render/interface/intf_render_backend.h>

namespace velk {

/**
 * @brief Base interface for all 2D pixel surfaces (images, textures, windows).
 *
 * Provides dimensions and pixel format. Concrete subtypes include renderable
 * targets (IRenderTarget) and uploadable images (ISurface + IBuffer).
 *
 * Chain: IInterface -> IGpuResource -> ISurface
 */
class ISurface : public Interface<ISurface, IGpuResource>
{
public:
    /** @brief Returns the surface dimensions in pixels. */
    virtual uvec2 get_dimensions() const = 0;

    /** @brief Returns the pixel format. */
    virtual PixelFormat format() const = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_SURFACE_H
