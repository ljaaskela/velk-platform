#include "text_visual.h"

#include "../embedded/velk_text_glsl.h"

#include <velk/api/object_ref.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk-render/interface/intf_material.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-ui/instance_types.h>
#include <velk-ui/plugins/text/intf_text_plugin.h>

namespace velk::ui {

void TextVisual::set_font(const IFont::Ptr& font)
{
    font_ = Font(font);
    bind_font_material();
    cache_.invalidate();
    invoke_visual_changed();
}

void TextVisual::on_state_changed(string_view name, IMetadata& owner, Uid interfaceId)
{
    constexpr string_view names[] = {"text", "font_size", "layout"};
    if (has_state_changed<ITextVisual>(interfaceId, name, names)) {
        cache_.invalidate();
    }
    invoke_visual_changed();
}

void TextVisual::ensure_default_font()
{
    if (font_) {
        return;
    }
    font_ = get_default_font();
    if (font_) {
        bind_font_material();
    }
}

void TextVisual::bind_font_material()
{
    auto font = font_.as_ptr<IFont>();
    if (!font) {
        return;
    }
    auto material = font->get_material();
    if (!material) {
        return;
    }
    write_state<IVisual>(this, [&](IVisual::State& s) {
        set_object_ref(s.paint, material);
    });
}

IFont* TextVisual::ensure_layout(const rect& bounds) const
{
    auto* font = font_.as_ptr<IFont>().get();
    if (!font) {
        return nullptr;
    }

    auto text_state = read_state<ITextVisual>(this);

    CacheKey key{};
    if (text_state) {
        key.text_data = text_state->text.data();
        key.text_size = static_cast<uint32_t>(text_state->text.size());
        key.font_size = text_state->font_size;
        key.layout = text_state->layout;
    }
    key.bounds_width = bounds.width;

    if (cache_.changed(key)) {
        if (text_state && !text_state->text.empty()) {
            string_view text(text_state->text.data(), text_state->text.size());
            font->layout_text(text, text_state->font_size, text_state->layout,
                              bounds.width, layout_result_);
        } else {
            layout_result_ = {};
        }
    }
    return font;
}

aabb TextVisual::get_local_bounds(const rect& bounds) const
{
    auto* font = ensure_layout(bounds);
    if (!font || layout_result_.glyphs.empty()) {
        aabb out;
        out.position = {bounds.x, bounds.y, 0.f};
        out.extent = {bounds.width, bounds.height, 0.f};
        return out;
    }

    // Laid-out text lives in the rect
    //   [bounds.x .. bounds.x + total_width, bounds.y .. bounds.y + total_height]
    // after vertical alignment; overflow past bounds.width is legitimate
    // when the content is wider than the element. Report the union of
    // the element's own box and the laid-out text extent so neither
    // gets clipped out of the element's world_aabb.
    float w = ::velk::max(bounds.width,  layout_result_.total_width);
    float h = ::velk::max(bounds.height, layout_result_.total_height);
    aabb out;
    out.position = {bounds.x, bounds.y, 0.f};
    out.extent = {w, h, 0.f};
    return out;
}

vector<DrawEntry> TextVisual::get_draw_entries(const rect& bounds)
{
    ensure_default_font();
    auto* font = ensure_layout(bounds);
    if (!font) {
        return {};
    }

    auto text_state = read_state<ITextVisual>(this);

    auto visual_state = read_state<IVisual>(this);
    ::velk::color col = visual_state ? visual_state->color : ::velk::color::white();

    HAlign ha = text_state ? text_state->h_align : HAlign::Left;
    VAlign va = text_state ? text_state->v_align : VAlign::Top;

    float offset_y = bounds.y;
    switch (va) {
    case VAlign::Center: offset_y += (bounds.height - layout_result_.total_height) * 0.5f; break;
    case VAlign::Bottom: offset_y += bounds.height - layout_result_.total_height; break;
    default: break;
    }

    vector<DrawEntry> result;
    result.reserve(layout_result_.glyphs.size());

    for (auto& line : layout_result_.lines) {
        float offset_x = bounds.x;
        switch (ha) {
        case HAlign::Center: offset_x += (bounds.width - line.width) * 0.5f; break;
        case HAlign::Right:  offset_x += bounds.width - line.width; break;
        default: break;
        }

        for (uint32_t i = 0; i < line.glyph_count; ++i) {
            auto& pg = layout_result_.glyphs[line.first_glyph + i];

            DrawEntry entry{};
            entry.pipeline_key = 0;
            entry.bounds = {pg.pos.x + offset_x, pg.pos.y + offset_y,
                            pg.size.x, pg.size.y};

            TextInstance inst{};
            inst.pos = {pg.pos.x + offset_x, pg.pos.y + offset_y};
            inst.size = pg.size;
            inst.col = col;
            inst.glyph_index = pg.glyph_index;
            entry.set_instance(inst);

            result.push_back(entry);
        }
    }

    return result;
}

vector<IBuffer::Ptr> TextVisual::get_gpu_resources() const
{
    auto font = font_.as_ptr<IFont>();
    if (!font) {
        return {};
    }
    vector<IBuffer::Ptr> out;
    if (auto b = font->get_curve_buffer()) {
        out.push_back(std::move(b));
    }
    if (auto b = font->get_band_buffer()) {
        out.push_back(std::move(b));
    }
    if (auto b = font->get_glyph_buffer()) {
        out.push_back(std::move(b));
    }
    return out;
}

namespace {

// Shape-intersect snippet: rect hit + slug coverage threshold. Runs
// inside BVH traversal (shadow / bounce rays), so shadows cast by text
// track the glyph silhouette instead of the quad. `fwidth` isn't
// available in compute, so it's stubbed the same way text_material's
// fill snippet does.
constexpr string_view text_intersect_src = R"(
#define fwidth(x) vec2(1.0 / 32.0)
#include "velk_text.glsl"

layout(buffer_reference, std430) buffer TextIntersectData {
    VelkTextCurveBuffer curves;
    VelkTextBandBuffer bands;
    VelkTextGlyphBuffer glyphs;
};

bool velk_intersect_text_glyph(Ray ray, RtShape shape, out RayHit hit)
{
    if (!intersect_rect(ray, shape, hit)) return false;
    if (shape.material_data_addr == 0u) return true;
    TextIntersectData d = TextIntersectData(shape.material_data_addr);
    vec2 glyph_uv = vec2(hit.uv.x, 1.0 - hit.uv.y);
    float coverage = velk_text_coverage(glyph_uv, shape.shape_param,
                                         d.curves, d.bands, d.glyphs);
    return coverage >= 0.5;
}
)";

} // namespace

string_view TextVisual::get_shape_intersect_source() const
{
    return text_intersect_src;
}

string_view TextVisual::get_shape_intersect_fn_name() const
{
    return "velk_intersect_text_glyph";
}

void TextVisual::register_shape_intersect_includes(::velk::IRenderContext& ctx) const
{
    ctx.register_shader_include("velk_text.glsl", embedded::velk_text_glsl);
}

} // namespace velk::ui
