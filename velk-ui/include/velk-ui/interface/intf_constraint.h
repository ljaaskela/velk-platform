#ifndef VELK_UI_INTF_CONSTRAINT_H
#define VELK_UI_INTF_CONSTRAINT_H

#include <velk/interface/intf_hierarchy.h>
#include <velk/interface/intf_interface.h>

#include <velk-ui/interface/intf_element.h>
#include <velk-ui/interface/intf_trait.h>
#include <velk-ui/types.h>

namespace velk_ui {

/**
 * @brief Determines when a constraint runs during the layout solve.
 *
 * Layout-phase constraints (e.g. Stack) run first and may walk children.
 * Constraint-phase constraints (e.g. FixedSize) run second and touch only self.
 */
enum class ConstraintPhase : uint8_t
{
    Layout,    ///< Runs first. May read/write children via hierarchy.
    Constraint ///< Runs second. Touches only the element itself.
};

/**
 * @brief Interface for layout constraints attached to elements.
 *
 * The solver calls measure() in a top-down pass to compute desired sizes,
 * then apply() to write final bounds into element state. Layout-phase
 * constraints receive the hierarchy so they can walk children; constraint-phase
 * constraints ignore it.
 */
class IConstraint : public velk::Interface<IConstraint, ITrait>
{
public:
    /** @brief Returns when this constraint should run relative to others. */
    virtual ConstraintPhase get_phase() const = 0;

    /**
     * @brief Computes the element's desired size within the given bounds.
     * @param c         Available space from the parent.
     * @param element   The element this constraint is attached to.
     * @param hierarchy The logical hierarchy (for layout constraints that walk children).
     * @return          Constraint with the measured bounds.
     */
    virtual Constraint measure(const Constraint& c, IElement& element, velk::IHierarchy* hierarchy) = 0;

    /**
     * @brief Writes final position and size into the element's state.
     * @param c         The resolved constraint from measure().
     * @param element   The element this constraint is attached to.
     * @param hierarchy The logical hierarchy (for layout constraints that position children).
     */
    virtual void apply(const Constraint& c, IElement& element, velk::IHierarchy* hierarchy) = 0;
};

} // namespace velk_ui

#endif // VELK_UI_INTF_CONSTRAINT_H
