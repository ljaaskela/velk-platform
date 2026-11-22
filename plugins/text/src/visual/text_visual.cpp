#include "text_visual.h"

#include "../embedded/velk_text_glsl.h"

#include <velk/api/object_ref.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/interface/resource/intf_resource.h>
#include <velk-render/interface/material/intf_material.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-scene/instance_types.h>
#include <velk-ui/plugins/text/intf_text_plugin.h>

namespace velk::ui {

void TextVisual::on_state_changed(string_view name, IMetadata& owner, Uid interfaceId)
{
    constexpr string_view font_names[] = {"font"};
    if (has_state_changed<ITextVisual>(interfaceId, name, font_names)) {
        update_font();
    }
    constexpr string_view names[] = {"text", "font_size", "layout"};
    if (has_state_changed<ITextVisual>(interfaceId, name, names)) {
        cache_.invalidate();
    }
    invoke_visual_changed();
}

namespace text_detail {

IFont::Ptr resolve_font(const ObjectRef& ref)
{
    auto font = ref.get<IFont>();
    auto* resource = interface_cast<IResource>(font);
    if (!font || (resource && !resource->exists())) {
        font = get_default_font();
    }
    return font;
}

void bind_font_material(IVisual2D* visual, const IFont::Ptr& font)
{
    auto material = font ? font->get_material() : nullptr;
    if (!material) {
        return;
    }
    write_state<IVisual2D>(visual, [&](IVisual2D::State& s) { set_object_ref(s.paint, material); });
}

void append_glyphs(const IFont::TextLayoutResult& layout, const ::velk::size& bounds, HAlign ha, VAlign va,
                   ::velk::color col, const IProgram::Ptr& material, float shift_x, float shift_y,
                   vector<DrawEntry>& out)
{
    float offset_y = shift_y;
    switch (va) {
    case VAlign::Center: offset_y += (bounds.height - layout.total_height) * 0.5f; break;
    case VAlign::Bottom: offset_y += bounds.height - layout.total_height; break;
    default: break;
    }

    out.reserve(out.size() + layout.glyphs.size());
    for (auto& line : layout.lines) {
        float offset_x = shift_x;
        switch (ha) {
        case HAlign::Center: offset_x += (bounds.width - line.width) * 0.5f; break;
        case HAlign::Right:  offset_x += bounds.width - line.width; break;
        default: break;
        }

        for (uint32_t i = 0; i < line.glyph_count; ++i) {
            auto& pg = layout.glyphs[line.first_glyph + i];
            const float x = pg.pos.x + offset_x;
            const float y = pg.pos.y + offset_y;

            DrawEntry entry{};
            entry.pipeline_key = 0;
            entry.bounds = {x, y, pg.size.x, pg.size.y};
            entry.material = material;

            ElementInstance inst{};
            inst.offset = {x, y, 0.f, 0.f};
            inst.size = {pg.size.x, pg.size.y, 0.f, 0.f};
            inst.col = col;
            inst.params[0] = pg.glyph_index;
            entry.set_instance(inst);

            out.push_back(entry);
        }
    }
}

} // namespace text_detail

void TextVisual::update_font()
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
    cache_.invalidate();
}

size TextVisual::content_size(const size& available)
{
    if (!font_) {
        update_font();
    }
    auto font = font_.as_ptr<IFont>();
    auto state = read_state<ITextVisual>(this);
    if (!font || !state || state->text.empty()) {
        return {};
    }
    const float width = state->layout == TextLayout::WordWrap ? available.width : 0.f;
    IFont::TextLayoutResult result;
    font->layout_text(string_view(state->text.data(), state->text.size()), state->font_size, state->layout, width,
                      result);
    return {result.total_width, result.total_height, 0.f};
}

IFont* TextVisual::ensure_layout(const ::velk::size& bounds) const
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

aabb TextVisual::get_local_bounds(const ::velk::size& bounds) const
{
    auto* font = ensure_layout(bounds);
    if (!font || layout_result_.glyphs.empty()) {
        aabb out;
        out.position = {0.f, 0.f, 0.f};
        out.extent = {bounds.width, bounds.height, bounds.depth};
        return out;
    }

    // Laid-out text lives in the rect
    //   [0 .. total_width, 0 .. total_height]
    // after vertical alignment; overflow past bounds.width is legitimate
    // when the content is wider than the element. Report the union of
    // the element's own box and the laid-out text extent so neither
    // gets clipped out of the element's world_aabb.
    float w = ::velk::max(bounds.width,  layout_result_.total_width);
    float h = ::velk::max(bounds.height, layout_result_.total_height);
    aabb out;
    out.position = {0.f, 0.f, 0.f};
    out.extent = {w, h, bounds.depth};
    return out;
}

vector<DrawEntry> TextVisual::get_draw_entries(::velk::IRenderContext& /*ctx*/,
                                                const ::velk::size& bounds)
{
    if (!font_) {
        update_font();
    }
    auto* font = ensure_layout(bounds);
    if (!font) {
        return {};
    }

    auto text_state = read_state<ITextVisual>(this);

    auto visual_state = read_state<IVisual2D>(this);
    ::velk::color col = visual_state ? visual_state->color : ::velk::color::white();

    HAlign ha = text_state ? text_state->h_align : HAlign::Left;
    VAlign va = text_state ? text_state->v_align : VAlign::Top;

    IProgram::Ptr material;
    if (visual_state && visual_state->paint) {
        material = visual_state->paint.get<IProgram>();
    }

    vector<DrawEntry> result;
    text_detail::append_glyphs(layout_result_, bounds, ha, va, col, material, 0.f, 0.f, result);
    return result;
}

vector<IBuffer::Ptr> TextVisual::get_gpu_resources(::velk::IRenderContext& ctx) const
{
    // The font's glyph data lives in the shared text arenas rather than in
    // per-font buffers, so there is nothing for the renderer to upload. This
    // hook is still the per-frame point where a render context is in hand,
    // which is what the font needs to (re)claim its regions after baking.
    if (auto font = font_.as_ptr<IFont>()) {
        font->ensure_gpu_data(ctx);
    }
    return {};
}

namespace {

// Shape-intersect snippet: rect hit + slug coverage threshold. Runs
// inside BVH traversal (shadow / bounce rays), so shadows cast by text
// track the glyph silhouette instead of the quad. `fwidth` isn't
// available in compute, so it's replaced the same way text_material's
// fill snippet does, or stubbed in passes without a footprint.
constexpr string_view text_intersect_src = R"(
#ifdef VELK_COMPUTE
#define fwidth(x) velk_uv_fwidth()
#else
#define fwidth(x) vec2(1.0 / 32.0)
#endif
#include "velk_text.glsl"

bool velk_intersect_text_glyph(Ray ray, RtShape shape, out RayHit hit)
{
    if (!intersect_rect(ray, shape, hit)) return false;
    if (shape.material_id == 0u) return true;
    // The shape carries the word base of its TextMaterial record. The three
    // bases are read straight out of the material arena rather than through
    // the material's generated reader: intersect snippets and material
    // snippets have independent active sets, so a frame can compose this
    // intersect without the text material's declarations being present.
    uint b = shape.material_base;
    uint curve_base = velk_material_words.data[b + 0u];
    uint band_base  = velk_material_words.data[b + 1u];
    uint glyph_base = velk_material_words.data[b + 2u];
    vec2 glyph_uv = vec2(hit.uv.x, 1.0 - hit.uv.y);
    float coverage = velk_text_coverage(glyph_uv, shape.shape_param,
                                        curve_base, band_base, glyph_base);
    return coverage >= 0.5;
}
)";

} // namespace

string_view text_detail::intersect_source()
{
    return text_intersect_src;
}

string_view text_detail::intersect_fn_name()
{
    return "velk_intersect_text_glyph";
}

string_view TextVisual::get_source(::velk::string_view role) const
{
    if (role == ::velk::shader_role::kIntersect) return text_detail::intersect_source();
    return {};
}

string_view TextVisual::get_fn_name(::velk::string_view role) const
{
    if (role == ::velk::shader_role::kIntersect) return text_detail::intersect_fn_name();
    return {};
}

void TextVisual::register_includes(::velk::IRenderContext& ctx) const
{
    ctx.shaders().register_include("velk_text.glsl", embedded::velk_text_glsl);
}

} // namespace velk::ui
