#ifndef VELK_UI_SCROLL_VIEW_H
#define VELK_UI_SCROLL_VIEW_H

#include <velk-scene/ext/trait.h>
#include <velk-scene/interface/trait/intf_clip.h>
#include <velk-ui/interface/intf_input_trait.h>
#include <velk-ui/interface/intf_layout_trait.h>
#include <velk-ui/interface/trait/intf_scroll_view.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/**
 * @brief See IScrollView. Lays out the children, clips them, takes wheel and drag input and
 *        draws the scroll bars, in one attachment.
 */
class ScrollView : public ::velk::ext::Visual2D<ScrollView, IScrollView, IClip, ILayoutTrait, IInputTrait>
{
public:
    VELK_CLASS_UID(ClassId::Constraint::ScrollView, "ScrollView");

    TraitPhase get_phase() const override
    {
        return TraitPhase::Layout | TraitPhase::Input | TraitPhase::Visual;
    }

    // ILayoutTrait
    void describe(LayoutBox&) const override {}
    size measure(const size& available, IElement& element, ILayoutContext& ctx) override;
    void arrange(const aabb& content, IElement& element, ILayoutContext& ctx) override;

    // IClip: the content area, without the bars.
    rect clip_rect(const size& box) const override;

    // IVisual
    vector<DrawEntry> get_draw_entries(::velk::IRenderContext& ctx, const ::velk::size& bounds) override;

    // IInputTrait
    InputResult on_intercept(PointerEvent&) override { return InputResult::Ignored; }
    InputResult on_pointer_event(PointerEvent& event) override;
    void on_pointer_enter(const PointerEvent&) override {}
    void on_pointer_leave(const PointerEvent&) override {}
    InputResult on_scroll_event(ScrollEvent& event) override;
    InputResult on_key_event(KeyEvent&) override { return InputResult::Ignored; }

protected:
    void on_state_changed(string_view name, IMetadata& owner, Uid interfaceId) override;

private:
    /// One axis: how much shows, how much there is, and the offset in use.
    struct Axis
    {
        float view = 0.f;    ///< Visible length of the content.
        float content = 0.f; ///< Length of the content.
        float offset = 0.f;  ///< Scroll offset, within [0, max()].
        float origin = 0.f;  ///< Where the content rectangle starts, in the element.
        bool bar = false;    ///< The content does not fit, so the axis has a bar.

        float max() const { return content > view ? content - view : 0.f; }
    };

    /// The bar's track and thumb along an axis, in element space.
    struct Bar
    {
        float track_start = 0.f;
        float track_length = 0.f;
        float thumb_start = 0.f;
        float thumb_length = 0.f;
    };
    Bar bar_of(const Axis& axis) const;
    /** @brief Writes the offset of @p vertical's axis, clamped; returns true if it changed. */
    bool scroll_to(bool vertical, float offset);

    Axis x_;
    Axis y_;
    /// Cross-axis position of each bar: the vertical bar's x, the horizontal bar's y.
    float v_bar_x_ = 0.f;
    float h_bar_y_ = 0.f;
    // Thumb dragging.
    bool dragging_ = false;
    bool drag_vertical_ = true;
    float drag_start_pointer_ = 0.f;
    float drag_start_offset_ = 0.f;
};

} // namespace velk::ui

#endif // VELK_UI_SCROLL_VIEW_H
