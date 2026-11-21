#ifndef VELK_UI_INSETS_H
#define VELK_UI_INSETS_H

#include <velk-ui/ext/trait.h>
#include <velk-ui/interface/trait/intf_insets.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

class Padding : public ext::LayoutTrait<Padding, TraitPhase::Constraint, IPadding>
{
public:
    VELK_CLASS_UID(ClassId::Constraint::Padding, "Padding");

    void describe(LayoutBox& box) const override;
};

class Margin : public ext::LayoutTrait<Margin, TraitPhase::Constraint, IMargin>
{
public:
    VELK_CLASS_UID(ClassId::Constraint::Margin, "Margin");

    void describe(LayoutBox& box) const override;
};

class Align : public ext::LayoutTrait<Align, TraitPhase::Constraint, IAlign>
{
public:
    VELK_CLASS_UID(ClassId::Constraint::Align, "Align");

    void describe(LayoutBox& box) const override;
};

} // namespace velk::ui

#endif // VELK_UI_INSETS_H
