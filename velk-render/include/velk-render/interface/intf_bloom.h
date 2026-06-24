#ifndef VELK_RENDER_INTF_BLOOM_H
#define VELK_RENDER_INTF_BLOOM_H

#include <velk/interface/intf_interface.h>
#include <velk/interface/intf_metadata.h>
#include <velk/uid.h>

namespace velk {

/**
 * @brief State interface for the bloom effect.
 *
 * Bloom extracts the bright (HDR) part of the image, blurs it across a
 * progressive mip chain, and adds the soft result back, so emitters and
 * highlights bleed a glow into their surroundings. Composed onto the bloom
 * effect so the look can be tuned at runtime through the state system,
 * which handles the handoff to the render thread.
 *
 * - `threshold` / `knee`: the soft-knee luminance cutoff. Pixels below
 *   `threshold` contribute nothing; `knee` widens the rolloff so the
 *   transition is gradual rather than a hard clip.
 * - `intensity`: how strongly the blurred glow is added back (0 = off).
 * - `radius`: scales the upsample tent filter; larger spreads the glow wider.
 */
class IBloom
    : public Interface<IBloom, IInterface,
                       VELK_UID("f0af35f6-4f14-4053-a1bf-8e9f5b5917b7")>
{
public:
    VELK_INTERFACE(
        (PROP, float, threshold, 1.f), ///< Luminance below which a pixel does not bloom.
        (PROP, float, knee,      0.5f), ///< Soft-knee width around the threshold.
        (PROP, float, intensity, 0.05f),///< Strength the glow is added back at (0 = off).
        (PROP, float, radius,    1.f)   ///< Upsample tent-filter scale; larger = wider glow.
    )
};

} // namespace velk

#endif // VELK_RENDER_INTF_BLOOM_H
