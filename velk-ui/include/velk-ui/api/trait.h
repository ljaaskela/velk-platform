#ifndef VELK_UI_API_TRAIT_H
#define VELK_UI_API_TRAIT_H

#include <velk/api/object.h>

#include <velk-ui/interface/intf_trait.h>

namespace velk::ui {

/**
 * @brief Base API wrapper for all element traits (constraints, visuals, etc.).
 *
 * Inherits Object and validates that the wrapped object implements ITrait.
 * Concrete wrappers (Stack, FixedSize, RectVisual) inherit from this.
 */
class Trait : public Object
{
public:
    /** @brief Default-constructed Trait wraps no object. */
    Trait() = default;

    /** @brief Wraps an existing IObject pointer, rejected if it does not implement ITrait. */
    explicit Trait(IObject::Ptr obj) : Object(check_object<ITrait>(obj)) {}

    explicit Trait(ITrait::Ptr t) : Object(as_object(t)) {}

    /** @brief Implicit conversion to ITrait::Ptr. */
    operator ITrait::Ptr() const { return as_ptr<ITrait>(); }
};

} // namespace velk::ui

#endif // VELK_UI_API_TRAIT_H
