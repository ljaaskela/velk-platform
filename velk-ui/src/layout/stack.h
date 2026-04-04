#ifndef VELK_UI_STACK_H
#define VELK_UI_STACK_H

#include <velk-ui/ext/trait.h>
#include <velk-ui/interface/trait/intf_stack.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

class Stack : public ext::Layout<Stack, TraitPhase::Layout, IStack>
{
public:
    VELK_CLASS_UID(ClassId::Constraint::Stack, "Stack");

    Constraint measure(const Constraint& c, IElement& element, IHierarchy& hierarchy) override;
    void apply(const Constraint& c, IElement& element, IHierarchy& hierarchy) override;
};

} // namespace velk::ui

#endif // VELK_UI_STACK_H
