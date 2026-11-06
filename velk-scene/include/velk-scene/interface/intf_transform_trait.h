#ifndef VELK_UI_INTF_TRANSFORM_TRAIT_H
#define VELK_UI_INTF_TRANSFORM_TRAIT_H

#include <velk-scene/interface/intf_element.h>
#include <velk-scene/interface/intf_trait.h>

namespace velk {

/**
 * @brief Trait that modifies the world matrix after layout.
 *
 * The solver computes the base world matrix from the layout position and
 * passes it through each transform trait in turn. Traits return the result and
 * do not write to the element; the solver writes the final matrix once.
 */
class ITransformTrait : public Interface<ITransformTrait, ITrait>
{
public:
    /**
     * @brief Returns the world matrix after applying this trait.
     * @param element The element being transformed.
     * @param world   The world matrix from layout and any earlier traits.
     */
    virtual mat4 transform(IElement& element, const mat4& world) = 0;

    /**
     * @brief Whether the result depends on another element's state.
     *
     * The solver re-evaluates such traits on every solve that does any work,
     * after the changed elements, since the other element may have moved.
     */
    virtual bool depends_on_other_elements() const = 0;
};

} // namespace velk

#endif // VELK_UI_INTF_TRANSFORM_TRAIT_H
