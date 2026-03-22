#ifndef VELK_UI_STACK_H
#define VELK_UI_STACK_H

#include <velk/ext/object.h>

#include <velk-ui/interface/constraint/intf_stack.h>
#include <velk-ui/interface/intf_constraint.h>
#include <velk-ui/plugin.h>

namespace velk_ui {

class Stack : public velk::ext::Object<Stack, IConstraint, IStack>
{
public:
    VELK_CLASS_UID(ClassId::Constraint::Stack, "Stack");

    ConstraintPhase get_phase() const override;
    Constraint measure(const Constraint& c, IElement& element, velk::IHierarchy* hierarchy) override;
    void apply(const Constraint& c, IElement& element, velk::IHierarchy* hierarchy) override;
};

} // namespace velk_ui

#endif // VELK_UI_STACK_H
