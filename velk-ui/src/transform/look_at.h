#ifndef VELK_UI_LOOK_AT_TRANSFORM_H
#define VELK_UI_LOOK_AT_TRANSFORM_H

#include <velk-ui/ext/trait.h>
#include <velk-ui/interface/trait/intf_look_at.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

class LookAt : public ext::Transform<LookAt, ILookAt>
{
public:
    VELK_CLASS_UID(ClassId::Transform::LookAt, "LookAt");

    void transform(IElement& element) override;
};

} // namespace velk::ui

#endif // VELK_UI_LOOK_AT_TRANSFORM_H
