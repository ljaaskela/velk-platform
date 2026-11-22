#include "text_field.h"

#include "../embedded/velk_text_glsl.h"
#include "text_visual.h"

#include <velk/api/event.h>
#include <velk/api/state.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-scene/instance_types.h>

namespace velk::ui {

namespace {

constexpr float kCaretWidth = 1.5f;

bool is_continuation(char c)
{
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

size_t prev_boundary(const string& s, size_t i)
{
    if (i == 0) {
        return 0;
    }
    --i;
    while (i > 0 && is_continuation(s[i])) {
        --i;
    }
    return i;
}

size_t next_boundary(const string& s, size_t i)
{
    if (i >= s.size()) {
        return s.size();
    }
    ++i;
    while (i < s.size() && is_continuation(s[i])) {
        ++i;
    }
    return i;
}

/** @brief Appends @p cp to @p out as UTF-8; returns the bytes written. */
size_t encode_utf8(char32_t cp, char* out)
{
    if (cp < 0x80) {
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp < 0x110000) {
        out[0] = static_cast<char>(0xF0 | (cp >> 18));
        out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[3] = static_cast<char>(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

} // namespace

void TextField::on_state_changed(string_view name, IMetadata& owner, Uid interfaceId)
{
    constexpr string_view font_names[] = {"font"};
    if (has_state_changed<ITextVisual>(interfaceId, name, font_names)) {
        update_font();
    }
    constexpr string_view text_names[] = {"text"};
    if (has_state_changed<ITextVisual>(interfaceId, name, text_names)) {
        if (auto state = read_state<ITextVisual>(this)) {
            text_ = state->text;
        }
        if (!editing_) {
            buffer_changed();
        }
    }
    constexpr string_view size_names[] = {"font_size"};
    if (has_state_changed<ITextVisual>(interfaceId, name, size_names)) {
        buffer_changed();
    }
    (void)owner;
    invoke_visual_changed();
}

void TextField::update_font()
{
    ObjectRef ref;
    if (auto state = read_state<ITextVisual>(this)) {
        ref = state->font;
    }
    auto font = text_detail::resolve_font(ref);
    if (font == font_.as_ptr<IFont>()) {
        return;
    }
    font_ = Font(font);
    text_detail::bind_font_material(this, font);
    buffer_changed();
}

float TextField::font_size() const
{
    auto state = read_state<ITextVisual>(this);
    return state ? state->font_size : 16.f;
}

float TextField::scale() const
{
    auto font = font_.as_ptr<IFont>();
    if (!font) {
        return 0.f;
    }
    auto state = read_state<IFont>(font);
    return state && state->units_per_em > 0.f ? font_size() / state->units_per_em : 0.f;
}

const string& TextField::shown() const
{
    return editing_ ? buffer_ : text_;
}

void TextField::buffer_changed()
{
    if (!font_) {
        update_font();
    }
    layout_dirty_ = true;
    offsets_.clear();
    stops_.clear();
    auto font = font_.as_ptr<IFont>();
    const float s = scale();
    const string& text = shown();
    vector<IFont::GlyphPosition> glyphs;
    for (size_t i = 0;; i = next_boundary(text, i)) {
        offsets_.push_back(i);
        stops_.push_back(font && i > 0 ? font->shape_text(string_view(text.data(), i), glyphs) * s : 0.f);
        if (i >= text.size()) {
            break;
        }
    }
}

float TextField::caret_x(size_t index) const
{
    for (size_t k = 0; k < offsets_.size(); ++k) {
        if (offsets_[k] >= index) {
            return stops_[k];
        }
    }
    return stops_.empty() ? 0.f : stops_.back();
}

size_t TextField::index_at(float x) const
{
    const float target = x + scroll_;
    size_t best = 0;
    float best_distance = -1.f;
    for (size_t k = 0; k < offsets_.size(); ++k) {
        const float d = stops_[k] > target ? stops_[k] - target : target - stops_[k];
        if (best_distance < 0.f || d < best_distance) {
            best_distance = d;
            best = offsets_[k];
        }
    }
    return best;
}

void TextField::set_editing(bool editing)
{
    editing_ = editing;
    write_state<ITextField>(this, [editing](ITextField::State& s) { s.editing = editing; });
}

void TextField::begin_edit()
{
    if (editing_) {
        return;
    }
    buffer_ = text_;
    caret_ = buffer_.size();
    set_editing(true);
    buffer_changed();
    invoke_visual_changed();
}

void TextField::end_edit(bool keep)
{
    if (!editing_) {
        return;
    }
    set_editing(false);
    scroll_ = 0.f;
    if (keep && buffer_ != text_) {
        string value = buffer_;
        write_state<ITextVisual>(this, [&](ITextVisual::State& s) { s.text = value; });
        invoke_event(get_interface(IInterface::UID), "on_commit", value);
    }
    buffer_changed();
    invoke_visual_changed();
}

void TextField::on_focus_changed(bool focused)
{
    if (focused) {
        begin_edit();
    } else {
        end_edit(true);
    }
}

InputResult TextField::on_pointer_event(PointerEvent& event)
{
    if (event.button != PointerButton::Left && event.action != PointerAction::Move) {
        return InputResult::Ignored;
    }
    if (event.action == PointerAction::Down) {
        begin_edit();
        caret_ = index_at(event.local_position.x);
        invoke_visual_changed();
        return InputResult::Consumed;
    }
    if (event.action == PointerAction::Up) {
        return InputResult::Consumed;
    }
    return InputResult::Ignored;
}

InputResult TextField::on_key_event(KeyEvent& event)
{
    if (!editing_) {
        return InputResult::Ignored;
    }
    // Shortcuts (Ctrl, Alt, Super) and key releases go on to the application.
    const bool shortcut = !!(event.modifiers & (Modifier::Ctrl | Modifier::Alt | Modifier::Super));
    if (event.action == KeyAction::Text) {
        const char32_t cp = event.codepoint;
        if (cp >= 0x20 && cp != 0x7F) {
            char bytes[4];
            const size_t n = encode_utf8(cp, bytes);
            buffer_.insert(caret_, string_view(bytes, n));
            caret_ += n;
            buffer_changed();
            invoke_visual_changed();
        }
        return InputResult::Consumed;
    }
    if (event.action == KeyAction::Up || shortcut) {
        return InputResult::Ignored;
    }

    switch (event.named) {
    case Key::Backspace:
        if (caret_ > 0) {
            const size_t from = prev_boundary(buffer_, caret_);
            buffer_.erase(from, caret_ - from);
            caret_ = from;
            buffer_changed();
        }
        break;
    case Key::Delete:
        if (caret_ < buffer_.size()) {
            buffer_.erase(caret_, next_boundary(buffer_, caret_) - caret_);
            buffer_changed();
        }
        break;
    case Key::Left: caret_ = prev_boundary(buffer_, caret_); break;
    case Key::Right: caret_ = next_boundary(buffer_, caret_); break;
    case Key::Home: caret_ = 0; break;
    case Key::End: caret_ = buffer_.size(); break;
    case Key::Enter:
        if (event.action == KeyAction::Down) {
            end_edit(true);
        }
        break;
    case Key::Escape:
        if (event.action == KeyAction::Down) {
            end_edit(false);
        }
        break;
    case Key::Unknown:
        // A character key: its text arrives as a separate KeyAction::Text event.
        return InputResult::Consumed;
    default: return InputResult::Ignored;
    }
    invoke_visual_changed();
    return InputResult::Consumed;
}

size TextField::content_size(const size&)
{
    if (!font_) {
        update_font();
    }
    auto font = font_.as_ptr<IFont>();
    if (!font) {
        return {};
    }
    auto state = read_state<IFont>(font);
    const float width = stops_.empty() ? 0.f : stops_.back();
    return {width + kCaretWidth, state ? state->line_height * scale() : 0.f, 0.f};
}

vector<DrawEntry> TextField::get_draw_entries(::velk::IRenderContext& ctx, const ::velk::size& bounds)
{
    if (!font_) {
        update_font();
    }
    auto font = font_.as_ptr<IFont>();
    if (!font) {
        return {};
    }
    bounds_ = bounds;

    const float fs = font_size();
    if (layout_dirty_) {
        const string& text = shown();
        if (text.empty()) {
            layout_ = {};
        } else {
            font->layout_text(string_view(text.data(), text.size()), fs, TextLayout::MultiLine, 0.f, layout_);
        }
        layout_dirty_ = false;
    }

    // Keep the caret in view, and no more scrolled than the text needs.
    const float cx = editing_ ? caret_x(caret_) : 0.f;
    const float text_width = stops_.empty() ? 0.f : stops_.back();
    const float room = bounds.width - kCaretWidth;
    if (!editing_) {
        scroll_ = 0.f;
    } else {
        if (cx - scroll_ > room) {
            scroll_ = cx - room;
        }
        if (cx - scroll_ < 0.f) {
            scroll_ = cx;
        }
        const float max_scroll = text_width - room > 0.f ? text_width - room : 0.f;
        if (scroll_ > max_scroll) {
            scroll_ = max_scroll;
        }
    }

    auto text_state = read_state<ITextVisual>(this);
    auto visual_state = read_state<IVisual2D>(this);
    const ::velk::color col = visual_state ? visual_state->color : ::velk::color::white();
    const VAlign va = text_state ? text_state->v_align : VAlign::Top;
    IProgram::Ptr material;
    if (visual_state && visual_state->paint) {
        material = visual_state->paint.get<IProgram>();
    }

    vector<DrawEntry> result;
    text_detail::append_glyphs(layout_, bounds, HAlign::Left, va, col, material, -scroll_, 0.f, result);

    if (editing_) {
        auto font_state = read_state<IFont>(font);
        const float line_height = font_state ? font_state->line_height * scale() : fs;
        float y = 0.f;
        if (va == VAlign::Center) {
            y = (bounds.height - line_height) * 0.5f;
        } else if (va == VAlign::Bottom) {
            y = bounds.height - line_height;
        }
        const float x = cx - scroll_;

        DrawEntry caret{};
        caret.pipeline_key = PipelineKey::Default;
        if (auto quad = ctx.get_mesh_builder().get_rect_quad(); quad && !quad->get_primitives().empty()) {
            caret.primitive = quad->get_primitives()[0];
        }
        caret.bounds = {x, y, kCaretWidth, line_height};
        ElementInstance inst{};
        inst.offset = {x, y, 0.f, 0.f};
        inst.size = {kCaretWidth, line_height, 0.f, 0.f};
        inst.col = col;
        caret.set_instance(inst);
        result.push_back(caret);
    }
    return result;
}

vector<IBuffer::Ptr> TextField::get_gpu_resources(::velk::IRenderContext& ctx) const
{
    if (auto font = font_.as_ptr<IFont>()) {
        font->ensure_gpu_data(ctx);
    }
    return {};
}

string_view TextField::get_source(::velk::string_view role) const
{
    if (role == ::velk::shader_role::kIntersect) return text_detail::intersect_source();
    return {};
}

string_view TextField::get_fn_name(::velk::string_view role) const
{
    if (role == ::velk::shader_role::kIntersect) return text_detail::intersect_fn_name();
    return {};
}

void TextField::register_includes(::velk::IRenderContext& ctx) const
{
    ctx.shaders().register_include("velk_text.glsl", embedded::velk_text_glsl);
}

} // namespace velk::ui
