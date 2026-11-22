#ifndef VELK_UI_TREE_VIEW_H
#define VELK_UI_TREE_VIEW_H

#include "list_view.h"

#include <velk-ui/interface/trait/intf_tree_view.h>

#include <unordered_set>

namespace velk::ui {

/**
 * @brief See ITreeView. Keeps the shown rows as a flat list, updated per source change and per
 *        expand or collapse; rows, input and drawing as in ListView.
 */
class TreeView : public ::velk::ext::Visual2D<TreeView, ITreeView, ILayoutTrait, IInputTrait>
{
public:
    VELK_CLASS_UID(ClassId::Constraint::TreeView, "TreeView");

    TraitPhase get_phase() const override
    {
        return TraitPhase::Constraint | TraitPhase::Layout | TraitPhase::Input | TraitPhase::Visual;
    }

    // ILayoutTrait
    void describe(LayoutBox& box) const override;
    size measure(const size& available, IElement& element, ILayoutContext& ctx) override;
    void arrange(const aabb& content, IElement& element, ILayoutContext& ctx) override;

    // IVisual: the selected row's background and the + and - markers.
    vector<DrawEntry> get_draw_entries(::velk::IRenderContext& ctx, const ::velk::size& bounds) override;

    // IInputTrait
    InputResult on_intercept(PointerEvent&) override { return InputResult::Ignored; }
    InputResult on_pointer_event(PointerEvent& event) override;
    void on_pointer_enter(const PointerEvent&) override {}
    void on_pointer_leave(const PointerEvent&) override {}
    InputResult on_scroll_event(ScrollEvent&) override { return InputResult::Ignored; }
    InputResult on_key_event(KeyEvent&) override { return InputResult::Ignored; }

    /** @brief The shown rows, top to bottom. For tests. */
    const vector<ShownRow>& shown() const { return flat_; }

protected:
    void on_state_changed(string_view name, IMetadata& owner, Uid interfaceId) override;

private:
    IItemSource::Ptr source() const;
    float row_height() const;
    float indent() const;
    void watch_source();

    bool is_expanded(const IObject::Ptr& item) const { return expanded_.count(item.get()) != 0; }
    /** @brief Appends @p item at @p depth and, while expanded, its shown descendants to @p out. */
    void append(IItemSource& s, const IObject::Ptr& item, uint32_t depth, vector<ShownRow>& out) const;
    void rebuild();
    void on_items_changed(const ItemChange& change);
    /** @brief Index in flat_ of @p parent's row, or -1 for the top level; -2 when not shown. */
    ptrdiff_t row_of(const IObject::Ptr& parent) const;
    /** @brief Index in flat_ where @p parent's child @p index starts (its end when past the last). */
    size_t child_start(ptrdiff_t parent_row, size_t index) const;
    /** @brief One past the last descendant of the row at @p row. */
    size_t subtree_end(size_t row) const;
    void toggle(size_t row);
    void changed();

    ScopedHandler source_changed_;
    IItemSource::WeakPtr watched_; ///< The source source_changed_ listens to.
    RowsCore rows_;
    vector<ShownRow> flat_;
    std::unordered_set<IObject*> expanded_; ///< Expanded items, by identity.
};

} // namespace velk::ui

#endif // VELK_UI_TREE_VIEW_H
