#ifndef VELK_UI_FIXED_SIZE_H
#define VELK_UI_FIXED_SIZE_H

#include <velk-ui/ext/trait.h>
#include <velk-ui/interface/trait/intf_fixed_size.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

class FixedSize : public ext::LayoutTrait<FixedSize, TraitPhase::Constraint, IFixedSize>
{
public:
    VELK_CLASS_UID(ClassId::Constraint::FixedSize, "FixedSize");

    Constraint measure(const Constraint& c, IElement& element, IHierarchy& hierarchy) override;
    void apply(const Constraint& c, IElement& element, IHierarchy& hierarchy) override;
};

} // namespace velk::ui

#endif // VELK_UI_FIXED_SIZE_H
