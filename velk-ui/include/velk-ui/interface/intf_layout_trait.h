#ifndef VELK_UI_INTF_LAYOUT_TRAIT_H
#define VELK_UI_INTF_LAYOUT_TRAIT_H

#include <velk/interface/intf_hierarchy.h>

#include <velk-ui/interface/intf_element.h>
#include <velk-ui/interface/intf_trait.h>
#include <velk-ui/types.h>

namespace velk::ui {

/**
 * @brief Trait that participates in layout (measure + apply).
 *
 * The solver calls measure() in a top-down pass to compute desired sizes,
 * then apply() to write final bounds into element state.
 */
class ILayoutTrait : public Interface<ILayoutTrait, ITrait>
{
public:
    /**
     * @brief Computes the element's desired size within the given bounds.
     * @param c         Available space from the parent.
     * @param element   The element this constraint is attached to.
     * @param hierarchy The logical hierarchy (for layout traits that walk children).
     * @return          Constraint with the measured bounds.
     */
    virtual Constraint measure(const Constraint& c, IElement& element, IHierarchy& hierarchy) = 0;

    /**
     * @brief Writes final position and size into the element's state.
     * @param c         The resolved constraint from measure().
     * @param element   The element this constraint is attached to.
     * @param hierarchy The logical hierarchy (for layout traits that position children).
     */
    virtual void apply(const Constraint& c, IElement& element, IHierarchy& hierarchy) = 0;
};

} // namespace velk::ui

#endif // VELK_UI_INTF_LAYOUT_TRAIT_H
