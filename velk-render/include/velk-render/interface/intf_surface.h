#ifndef VELK_RENDER_INTF_SURFACE_H
#define VELK_RENDER_INTF_SURFACE_H

#include <velk/interface/intf_metadata.h>

#include <velk-render/render_types.h>

namespace velk {

/**
 * @brief A render target with dimensions and pacing settings.
 *
 * Represents a swapchain surface. Created via IRenderContext::create_surface()
 * from a SurfaceConfig. Width and height are mutable properties (updated by
 * the platform layer on resize); update_rate and target_fps are read-only and
 * fixed at creation time.
 */
class ISurface : public Interface<ISurface>
{
public:
    VELK_INTERFACE(
        (PROP, int, width, 0),                                ///< Surface width in pixels (updated on resize).
        (PROP, int, height, 0),                               ///< Surface height in pixels (updated on resize).
        (RPROP, UpdateRate, update_rate, UpdateRate::VSync),  ///< Swapchain pacing mode (fixed at create time).
        (RPROP, int, target_fps, 60)                          ///< Target framerate for UpdateRate::Targeted.
    )
};

} // namespace velk

#endif // VELK_RENDER_INTF_SURFACE_H
