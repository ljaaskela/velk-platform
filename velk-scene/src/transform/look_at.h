#ifndef VELK_UI_LOOK_AT_TRANSFORM_H
#define VELK_UI_LOOK_AT_TRANSFORM_H

#include <velk-scene/ext/trait.h>
#include <velk-scene/interface/trait/intf_look_at.h>
#include <velk-scene/plugin.h>

namespace velk {

class LookAt : public ext::Transform<LookAt, ILookAt>
{
public:
    VELK_CLASS_UID(ClassId::Transform::LookAt, "LookAt");

    mat4 transform(IElement& element, const mat4& world) override;
    bool depends_on_other_elements() const override { return true; }
};

} // namespace velk

#endif // VELK_UI_LOOK_AT_TRANSFORM_H
