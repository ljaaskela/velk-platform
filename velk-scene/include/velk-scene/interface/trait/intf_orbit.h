#ifndef VELK_UI_INTF_ORBIT_H
#define VELK_UI_INTF_ORBIT_H

#include <velk/api/math_types.h>
#include <velk/interface/intf_metadata.h>

namespace velk {

/**
 * @brief Transform trait that positions and orients an element on a sphere around a target.
 *
 * Computes position from the target element's world position, distance,
 * yaw (horizontal angle), and pitch (vertical angle). The element is
 * oriented to face the target.
 */
class IOrbit : public Interface<IOrbit>
{
public:
    VELK_INTERFACE(
        (PROP, ObjectRef, target, {}),
        (PROP, float, distance, 500.f),
        (PROP, float, yaw, 0.f),
        (PROP, float, pitch, 0.f)
    )
};

} // namespace velk

#endif // VELK_UI_INTF_ORBIT_H
