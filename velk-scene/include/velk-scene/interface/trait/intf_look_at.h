#ifndef VELK_UI_INTF_LOOK_AT_H
#define VELK_UI_INTF_LOOK_AT_H

#include <velk/api/math_types.h>
#include <velk/interface/intf_metadata.h>

namespace velk {

/**
 * @brief Transform trait that orients an element to face a target.
 *
 * Keeps the element's position unchanged and rotates it to look at
 * the target element's world position (plus optional offset).
 */
class ILookAt : public Interface<ILookAt>
{
public:
    VELK_INTERFACE(
        (PROP, ObjectRef, target, {}),
        (PROP, vec3, target_offset, {})
    )
};

} // namespace velk

#endif // VELK_UI_INTF_LOOK_AT_H
