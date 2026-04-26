#ifndef VELK_UI_INTF_TRS_H
#define VELK_UI_INTF_TRS_H

#include <velk/api/math_types.h>
#include <velk/interface/intf_metadata.h>

namespace velk {

/**
 * @brief Decomposed transform: translate, rotate, scale.
 *
 * Applied as T * R * S after layout is finalized. Rotation is stored as
 * a quaternion (xyzw) so glTF-style assets round-trip losslessly. The
 * Trs API wrapper exposes both quaternion accessors and the existing
 * Euler-degree helpers for callers that author rotations by hand.
 */
class ITrs : public Interface<ITrs>
{
public:
    VELK_INTERFACE(
        (PROP, vec3, translate, {}),
        (PROP, quat, rotation, (quat::identity())),
        (PROP, vec3, scale, (vec3{1.f, 1.f, 1.f}))
    )
};

} // namespace velk

#endif // VELK_UI_INTF_TRS_H
