#ifndef VELK_UI_LIST_VIEW_H
#define VELK_UI_LIST_VIEW_H

#include <velk/api/event.h>
#include <velk/ext/object.h>
#include <velk/interface/intf_store.h>
#include <velk/plugins/data/interface/intf_item_source.h>
#include <velk-scene/ext/trait.h>
#include <velk-ui/interface/intf_input_trait.h>
#include <velk-ui/interface/intf_layout_trait.h>
#include <velk-ui/interface/intf_list_row.h>
#include <velk-ui/interface/trait/intf_list_view.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/** @brief See IListRow. Holds values only; the view writes them. */
class ListRow : public ::velk::ext::Object<ListRow, IListRow>
{
public:
    VELK_CLASS_UID(ClassId::ListRow, "ListRow");
};

/**
 * @brief See IListView. Sizes the element to its rows, creates and reuses rows for the part
 *        that shows, takes clicks and draws the selection, in one attachment.
 */
class ListView : public ::velk::ext::Visual2D<ListView, IListView, ILayoutTrait, IInputTrait>
{
public:
    VELK_CLASS_UID(ClassId::Constraint::ListView, "ListView");

    TraitPhase get_phase() const override
    {
        return TraitPhase::Constraint | TraitPhase::Layout | TraitPhase::Input | TraitPhase::Visual;
    }

    // ILayoutTrait
    void describe(LayoutBox& box) const override;
    size measure(const size& available, IElement& element, ILayoutContext& ctx) override;
    void arrange(const aabb& content, IElement& element, ILayoutContext& ctx) override;

    // IVisual: the selected row's background.
    vector<DrawEntry> get_draw_entries(::velk::IRenderContext& ctx, const ::velk::size& bounds) override;

    // IInputTrait
    InputResult on_intercept(PointerEvent&) override { return InputResult::Ignored; }
    InputResult on_pointer_event(PointerEvent& event) override;
    void on_pointer_enter(const PointerEvent&) override {}
    void on_pointer_leave(const PointerEvent&) override {}
    InputResult on_scroll_event(ScrollEvent&) override { return InputResult::Ignored; }
    InputResult on_key_event(KeyEvent&) override { return InputResult::Ignored; }

protected:
    void on_state_changed(string_view name, IMetadata& owner, Uid interfaceId) override;

private:
    struct Row
    {
        IElement::Ptr element;
        IListRow::Ptr row;
        IStore::Ptr store;   ///< The row's import; grafting it again restores the row's subtree.
        size_t index = 0;
    };

    IItemSource::Ptr source() const;
    float row_height() const;
    void watch_source();
    /** @brief A row from the pool or the template, under @p element; null when there is no template. */
    bool take_row(IElement& element, ILayoutContext& ctx, Row& out);
    /** @brief Takes every row out of the scene and drops the pool and the template. */
    void drop_rows(ILayoutContext* ctx);

    ScopedHandler source_changed_;
    string template_uri_;   ///< The template the text below was read from.
    string template_text_;  ///< Read once, imported once per row created.
    bool template_failed_ = false;
    vector<Row> active_;    ///< Rows in the scene, each showing one item.
    vector<Row> pool_;      ///< Rows out of the scene, kept for reuse.
    aabb content_{};        ///< Content rectangle of the last arrange.
    size_t shown_first_ = 0; ///< The range of indices shown by the last arrange.
    size_t shown_last_ = 0;
    bool drop_pending_ = false;
};

} // namespace velk::ui

#endif // VELK_UI_LIST_VIEW_H
