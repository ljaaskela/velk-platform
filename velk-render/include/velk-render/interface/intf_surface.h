#ifndef VELK_RENDER_INTF_SURFACE_H
#define VELK_RENDER_INTF_SURFACE_H

#include <velk/interface/intf_metadata.h>

namespace velk {

/** @brief A render target with dimensions. Represents a swapchain surface. */
class ISurface : public Interface<ISurface>
{
public:
    VELK_INTERFACE(
        (PROP, int, width, 0),  ///< Surface width in pixels.
        (PROP, int, height, 0)  ///< Surface height in pixels.
    )
};

} // namespace velk

#endif // VELK_RENDER_INTF_SURFACE_H
