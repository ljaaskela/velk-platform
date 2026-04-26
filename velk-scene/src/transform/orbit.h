#ifndef VELK_UI_ORBIT_TRANSFORM_H
#define VELK_UI_ORBIT_TRANSFORM_H

#include <velk-scene/ext/trait.h>
#include <velk-scene/interface/trait/intf_orbit.h>
#include <velk-scene/plugin.h>

namespace velk {

class Orbit : public ext::Transform<Orbit, IOrbit>
{
public:
    VELK_CLASS_UID(ClassId::Transform::Orbit, "Orbit");

    void transform(IElement& element) override;
};

} // namespace velk

#endif // VELK_UI_ORBIT_TRANSFORM_H
