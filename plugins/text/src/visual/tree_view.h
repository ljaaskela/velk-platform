#ifndef VELK_UI_TREE_VIEW_H
#define VELK_UI_TREE_VIEW_H

#include <velk/api/event.h>
#include <velk/interface/intf_hierarchy.h>

#include <velk-scene/ext/trait.h>
#include <velk-ui/interface/intf_content_size.h>
#include <velk-ui/interface/intf_font.h>
#include <velk-ui/interface/intf_input_trait.h>
#include <velk-ui/plugins/text/api/font.h>
#include <velk-ui/plugins/text/intf_tree_view.h>
#include <velk-ui/plugins/text/plugin.h>

namespace velk::ui {

/**
 * @brief See ITreeView. Draws every shown row (text, selection, +/-) and takes clicks, in one
 *        attachment. Every row is drawn; the GPU clips what a scroll view does not show.
 */
class TreeView : public ::velk::ext::Visual2D<TreeView, ITreeView, IContentSize, IInputTrait>
{
public:
    VELK_CLASS_UID(ClassId::Visual::TreeView, "TreeView");

    TraitPhase get_phase() const override { return TraitPhase::Visual | TraitPhase::Input; }

    // IContentSize
    size content_size(const size& available) override;

    // IVisual
    vector<DrawEntry> get_draw_entries(::velk::IRenderContext& ctx, const ::velk::size& bounds) override;
    vector<IBuffer::Ptr> get_gpu_resources(::velk::IRenderContext& ctx) const override;

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
        IObject::WeakPtr object;
        uint32_t depth = 0;
        bool has_children = false;
        bool expanded = false;
        string label;
        float text_width = 0.f;
    };

    /** @brief Rebuilds the rows from the hierarchy and the expanded objects. */
    void rebuild_rows();
    /** @brief Subscribes to the source hierarchy's changes. */
    void watch_source();
    bool is_expanded(const IObject::Ptr& object) const;
    void toggle(const IObject::Ptr& object);
    /** @brief Width of the +/- in front of a row. */
    float marker_width() const;

    Font font_;
    vector<Row> rows_;
    vector<IObject::WeakPtr> expanded_;
    IObject::WeakPtr expanded_root_; ///< The root the tree last started expanded from.
    ScopedHandler source_changed_;
    float content_width_ = 0.f;
};

} // namespace velk::ui

#endif // VELK_UI_TREE_VIEW_H
