#ifndef VELK_UI_TRS_H
#define VELK_UI_TRS_H

#include <velk-scene/ext/trait.h>
#include <velk-scene/interface/trait/intf_trs.h>
#include <velk-scene/plugin.h>

namespace velk {

class Trs : public ext::Transform<Trs, ITrs>
{
public:
    VELK_CLASS_UID(ClassId::Transform::Trs, "Trs");

    mat4 transform(IElement& element, const mat4& world) override;
};

} // namespace velk

#endif // VELK_UI_TRS_H
