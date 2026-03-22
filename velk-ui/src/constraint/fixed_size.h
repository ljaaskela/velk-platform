#ifndef VELK_UI_FIXED_SIZE_H
#define VELK_UI_FIXED_SIZE_H

#include <velk/ext/object.h>

#include <velk-ui/interface/constraint/intf_fixed_size.h>
#include <velk-ui/interface/intf_constraint.h>
#include <velk-ui/plugin.h>

namespace velk_ui {

class FixedSize : public velk::ext::Object<FixedSize, IConstraint, IFixedSize>
{
public:
    VELK_CLASS_UID(ClassId::Constraint::FixedSize, "FixedSize");

    ConstraintPhase get_phase() const override;
    Constraint measure(const Constraint& c, IElement& element, velk::IHierarchy* hierarchy) override;
    void apply(const Constraint& c, IElement& element, velk::IHierarchy* hierarchy) override;
};

} // namespace velk_ui

#endif // VELK_UI_FIXED_SIZE_H
