#ifndef VELK_UI_ORBIT_TRANSFORM_H
#define VELK_UI_ORBIT_TRANSFORM_H

#include <velk-ui/ext/trait.h>
#include <velk-ui/interface/trait/intf_orbit.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

class Orbit : public ext::Transform<Orbit, IOrbit>
{
public:
    VELK_CLASS_UID(ClassId::Transform::Orbit, "Orbit");

    void transform(IElement& element) override;
};

} // namespace velk::ui

#endif // VELK_UI_ORBIT_TRANSFORM_H
