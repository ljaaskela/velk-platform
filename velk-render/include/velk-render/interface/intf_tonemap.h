#ifndef VELK_RENDER_INTF_TONEMAP_H
#define VELK_RENDER_INTF_TONEMAP_H

#include <velk/interface/intf_interface.h>
#include <velk/interface/intf_metadata.h>
#include <velk/uid.h>

namespace velk {

/**
 * @brief State interface for the tonemap effect.
 *
 * Carries the exposure multiplier applied to HDR radiance before the ACES
 * curve. Composed onto the tonemap effect so it can be tuned at runtime
 * through the state system, which handles the handoff to the render thread.
 * exposure > 1 lifts the image, < 1 darkens it.
 */
class ITonemap
    : public Interface<ITonemap, IInterface,
                       VELK_UID("0963796b-20d8-4801-956a-90f858b7b427")>
{
public:
    VELK_INTERFACE(
        (PROP, float, exposure, 1.f) ///< Linear multiply applied before the ACES curve.
    )
};

} // namespace velk

#endif // VELK_RENDER_INTF_TONEMAP_H
