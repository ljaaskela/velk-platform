#ifndef VELK_UI_STACK_H
#define VELK_UI_STACK_H

#include <velk-ui/ext/trait.h>
#include <velk-ui/interface/trait/intf_stack.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

class Stack : public ext::LayoutTrait<Stack, TraitPhase::Layout, IStack>
{
public:
    VELK_CLASS_UID(ClassId::Constraint::Stack, "Stack");

    size measure(const size& available, IElement& element, ILayoutContext& ctx) override;
    void arrange(const aabb& content, IElement& element, ILayoutContext& ctx) override;
};

} // namespace velk::ui

#endif // VELK_UI_STACK_H
