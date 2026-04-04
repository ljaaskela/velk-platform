#ifndef VELK_UI_INTF_TRANSFORM_TRAIT_H
#define VELK_UI_INTF_TRANSFORM_TRAIT_H

#include <velk-ui/interface/intf_element.h>
#include <velk-ui/interface/intf_trait.h>

namespace velk::ui {

/**
 * @brief Trait that modifies the world matrix after layout.
 *
 * The solver calls transform() after computing the base world matrix
 * from layout position. Transform traits read, modify, and write back
 * the element's world_matrix.
 */
class ITransformTrait : public Interface<ITransformTrait, ITrait>
{
public:
    /** @brief Modifies the element's world matrix in place. */
    virtual void transform(IElement& element) = 0;
};

} // namespace velk::ui

#endif // VELK_UI_INTF_TRANSFORM_TRAIT_H
