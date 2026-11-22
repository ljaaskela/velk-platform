#ifndef VELK_UI_TEXT_FIELD_H
#define VELK_UI_TEXT_FIELD_H

#include <velk/api/object.h>

#include <velk-render/interface/intf_analytic_shape.h>
#include <velk-scene/ext/trait.h>
#include <velk-scene/interface/trait/intf_clip.h>
#include <velk-ui/interface/intf_content_size.h>
#include <velk-ui/interface/intf_focus_target.h>
#include <velk-ui/interface/intf_font.h>
#include <velk-ui/interface/intf_input_trait.h>
#include <velk-ui/plugins/text/api/font.h>
#include <velk-ui/plugins/text/intf_text_field.h>
#include <velk-ui/plugins/text/intf_text_visual.h>
#include <velk-ui/plugins/text/plugin.h>

namespace velk::ui {

/**
 * @brief Single-line editable text.
 *
 * A text visual and an input trait in one attachment: it draws the text (or the edit buffer
 * while editing) and a caret, and takes focus and key events from the input dispatcher. It is
 * also a clip, so text wider than the element is cut at its edges.
 */
class TextField : public ::velk::ext::Visual2D<TextField, ITextVisual, ITextField, ::velk::IAnalyticShape,
                                               IContentSize, IInputTrait, IFocusTarget, IClip>
{
public:
    VELK_CLASS_UID(ClassId::Visual::TextField, "TextField");

    TraitPhase get_phase() const override { return TraitPhase::Visual | TraitPhase::Input; }

    // IContentSize
    size content_size(const size& available) override;

    // IVisual
    vector<DrawEntry> get_draw_entries(::velk::IRenderContext& ctx, const ::velk::size& bounds) override;
    vector<IBuffer::Ptr> get_gpu_resources(::velk::IRenderContext& ctx) const override;

    // IAnalyticShape, IShaderSource: as TextVisual.
    uint32_t get_shape_kind() const override { return 0; }
    string_view get_source(::velk::string_view role) const override;
    string_view get_fn_name(::velk::string_view role) const override;
    void register_includes(::velk::IRenderContext& ctx) const override;

    // IInputTrait
    InputResult on_intercept(PointerEvent&) override { return InputResult::Ignored; }
    InputResult on_pointer_event(PointerEvent& event) override;
    void on_pointer_enter(const PointerEvent&) override {}
    void on_pointer_leave(const PointerEvent&) override {}
    InputResult on_scroll_event(ScrollEvent&) override { return InputResult::Ignored; }
    InputResult on_key_event(KeyEvent& event) override;

    // IFocusTarget
    void on_focus_changed(bool focused) override;

    // IClip
    rect clip_rect(const size& box) const override { return {0.f, 0.f, box.width, box.height}; }

protected:
    void on_state_changed(string_view name, IMetadata& owner, Uid interfaceId) override;

private:
    void update_font();
    /** @brief Starts editing with the buffer set from `text`. */
    void begin_edit();
    /** @brief Ends editing; with @p keep, writes the buffer to `text`. */
    void end_edit(bool keep);
    void set_editing(bool editing);
    /** @brief Recomputes the caret stops and the layout after the buffer changed. */
    void buffer_changed();
    /** @brief The text shown: the buffer while editing, else `text`. */
    const string& shown() const;
    /** @brief The caret position (byte offset) nearest to @p x, in field coordinates. */
    size_t index_at(float x) const;
    /** @brief The x of the caret at @p index, before scrolling. */
    float caret_x(size_t index) const;
    /** @brief Pixel scale of font units at the current font size. */
    float scale() const;
    float font_size() const;

    Font font_;
    string text_;    ///< Copy of `text`, so shown() can return a reference.
    string buffer_;  ///< The edited text.
    size_t caret_{}; ///< Byte offset of the caret in buffer_.
    float scroll_{}; ///< How far the text is moved left, in pixels.
    bool editing_{};
    /// x of each caret position: stops_[i] is the pen position after `offsets_[i]` bytes.
    vector<size_t> offsets_;
    vector<float> stops_;
    ::velk::size bounds_{};
    IFont::TextLayoutResult layout_;
    bool layout_dirty_ = true;
};

} // namespace velk::ui

#endif // VELK_UI_TEXT_FIELD_H
