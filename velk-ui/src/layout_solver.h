#ifndef VELK_UI_LAYOUT_SOLVER_H
#define VELK_UI_LAYOUT_SOLVER_H

#include <velk/api/math_types.h>

#include <unordered_map>
#include <velk/interface/intf_hierarchy.h>

#include <velk-scene/interface/intf_element.h>
#include <velk-scene/interface/intf_scene.h>
#include <velk-ui/interface/intf_layout_trait.h>

namespace velk::ui {

/**
 * @brief Resolves element sizes, positions, world matrices and bounds.
 *
 * Measures bottom-up (desired sizes) and arranges top-down (final rectangles),
 * running one box model over what each element's Constraint-phase traits
 * describe. Solves only what @p pending says changed, so the cost follows the
 * changed elements and their descendants rather than the size of the scene.
 * Every element whose world matrix or size changes is marked dirty, which is
 * how the renderer learns about descendants moved by a parent.
 */
class LayoutSolver final : public ILayoutContext
{
public:
    void solve(IHierarchy& hierarchy, const aabb& viewport, const PendingLayout& pending);

    // ILayoutContext
    IHierarchy& hierarchy() override { return *hierarchy_; }
    LayoutMeasure measure(IElement& child, const size& available) override;
    void arrange(IElement& child, const aabb& slot, Alignment default_x, Alignment default_y) override;
    rect visible() const override { return element_visible_; }
    bool changed(IElement& element) const override;

private:
    /** @brief The rectangle, in world x and y, that the children of @p element may show in. */
    rect children_visible(const IElement::Ptr& element) const;
    /** @brief Arranges @p element in the content rectangle of its parent, or in @p viewport at the root. */
    void arrange_from_parent(const IElement::Ptr& element, const aabb& viewport);

    /** @brief The size of @p element's content (children and visuals) for @p available space. */
    size measure_content(IElement& element, ILayoutTrait* layout, const size& available);

    /** @brief Updates a world matrix, descending only while the result changes. */
    void update_transform(const IElement::Ptr& element, const mat4& parent_world);

    IHierarchy* hierarchy_ = nullptr;
    mat4 parent_world_ = mat4::identity(); ///< World matrix of the element whose children are arranged.
    /// Where the children being arranged may show, in world x and y (translation only, like clips).
    rect parent_visible_{};
    rect element_visible_{};          ///< visible() of the element being arranged, in its own space.
    bool all_changed_ = false;        ///< A full layout: everything counts as changed.
    vector<IElement*> changed_;       ///< Sorted: changed elements and their ancestors.

    /// Content sizes from earlier layouts, reused while the element and its subtree are unchanged.
    struct Measured
    {
        size available;
        size content;
    };
    std::unordered_map<IElement*, Measured> measured_;
};

} // namespace velk::ui

#endif // VELK_UI_LAYOUT_SOLVER_H
