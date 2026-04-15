#ifndef VELK_RENDER_INTF_WINDOW_SURFACE_H
#define VELK_RENDER_INTF_WINDOW_SURFACE_H

#include <velk/interface/intf_metadata.h>

#include <velk-render/interface/intf_render_target.h>
#include <velk-render/render_types.h>

namespace velk {

/**
 * @brief A swapchain window surface with pacing settings.
 *
 * Created via IRenderContext::create_surface() from a SurfaceConfig.
 * Size is updated by the platform layer on resize; update_rate and
 * target_fps are fixed at creation time.
 *
 * Chain: IInterface -> IGpuResource -> ISurface -> IRenderTarget -> IWindowSurface
 */
class IWindowSurface : public Interface<IWindowSurface, IRenderTarget>
{
public:
    VELK_INTERFACE(
        (PROP, uvec2, size, {}),                              ///< Surface size in pixels (updated on resize).
        (RPROP, UpdateRate, update_rate, UpdateRate::VSync),  ///< Swapchain pacing mode (fixed at create time).
        (RPROP, int, target_fps, 60)                          ///< Target framerate for UpdateRate::Targeted.
    )
};

} // namespace velk

#endif // VELK_RENDER_INTF_WINDOW_SURFACE_H
