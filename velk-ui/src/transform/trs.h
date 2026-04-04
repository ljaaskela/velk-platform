#ifndef VELK_UI_TRS_H
#define VELK_UI_TRS_H

#include <velk-ui/ext/trait.h>
#include <velk-ui/interface/trait/intf_trs.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

class Trs : public ext::Transform<Trs, ITrs>
{
public:
    VELK_CLASS_UID(ClassId::Transform::Trs, "Trs");

    void transform(IElement& element) override;
};

} // namespace velk::ui

#endif // VELK_UI_TRS_H
