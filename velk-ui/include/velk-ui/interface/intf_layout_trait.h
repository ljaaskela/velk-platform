#ifndef VELK_UI_INTF_LAYOUT_TRAIT_H
#define VELK_UI_INTF_LAYOUT_TRAIT_H

#include <velk/interface/intf_hierarchy.h>

#include <velk-scene/interface/intf_element.h>
#include <velk-scene/interface/intf_trait.h>
#include <velk-scene/types.h>

namespace velk::ui {

/// Space on the four sides of a box. px, or pct of the space offered on that axis.
struct Insets
{
    dim left = dim::zero();
    dim top = dim::zero();
    dim right = dim::zero();
    dim bottom = dim::zero();
};

/**
 * @brief An element's box, as its Constraint-phase traits describe it.
 *
 * The solver runs one box model over it: margin outside, then the size, then
 * padding around the children.
 */
struct LayoutBox
{
    dim width = dim::none();  ///< none: the space offered; px, pct: fixed; auto: the content.
    dim height = dim::none();
    dim depth = dim::none();  ///< none: 0; px: fixed.
    Insets margin;
    Insets padding;
    Alignment align_x = Alignment::Unset;  ///< Unset: the parent's alignment.
    Alignment align_y = Alignment::Unset;
};

/// The size an element wants, margin included.
struct LayoutMeasure
{
    size desired{};
    bool fill_x = false; ///< Takes whatever width it is given.
    bool fill_y = false; ///< Takes whatever height it is given.
};

/**
 * @brief The layout solver, as Layout-phase traits see it.
 *
 * Positions and rectangles are in the parent element's space.
 */
class ILayoutContext
{
public:
    /** @brief The hierarchy being solved. */
    virtual IHierarchy& hierarchy() = 0;

    /** @brief Measures @p child for @p available space. */
    virtual LayoutMeasure measure(IElement& child, const size& available) = 0;

    /**
     * @brief Sizes and places @p child in @p slot and lays out its subtree.
     * @param slot      The space given to the child, margin included.
     * @param default_x Alignment used where the child sets none.
     * @param default_y Alignment used where the child sets none.
     */
    virtual void arrange(IElement& child, const aabb& slot, Alignment default_x, Alignment default_y) = 0;

protected:
    ~ILayoutContext() = default;
};

/**
 * @brief Trait that takes part in layout.
 *
 * Constraint-phase traits (FixedSize, Padding, Margin, Align) describe the
 * element's box. A Layout-phase trait (Stack) measures and places the
 * element's children; without one, children are measured as a pile and
 * placed at the start of the content rectangle.
 */
class ILayoutTrait : public Interface<ILayoutTrait, ITrait>
{
public:
    /** @brief Constraint phase: writes this trait's part of the element's box. */
    virtual void describe(LayoutBox& box) const = 0;

    /**
     * @brief Layout phase: measures the children.
     * @param available The element's content space.
     * @return The size the children need.
     */
    virtual size measure(const size& available, IElement& element, ILayoutContext& ctx) = 0;

    /**
     * @brief Layout phase: places the children.
     * @param content The element's content rectangle, in the element's own space.
     */
    virtual void arrange(const aabb& content, IElement& element, ILayoutContext& ctx) = 0;
};

} // namespace velk::ui

#endif // VELK_UI_INTF_LAYOUT_TRAIT_H
