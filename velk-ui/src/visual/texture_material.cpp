#include "texture_material.h"

#include "../renderer/default_ui_shaders.h"

#include <velk/api/state.h>
#include <velk-render/gpu_data.h>

#include <cstring>

namespace velk::ui::impl {

namespace {

VELK_GPU_STRUCT TextureParams
{
    ::velk::color tint;
};

constexpr string_view texture_eval_src = R"(
layout(buffer_reference, std430) readonly buffer TextureMaterialData {
    vec4 tint;
};

MaterialEval velk_eval_texture(EvalContext ctx)
{
    TextureMaterialData d = TextureMaterialData(ctx.data_addr);
    vec4 sampled = velk_texture(ctx.texture_id, ctx.uv);

    MaterialEval e;
    e.color = sampled * d.tint;
    e.normal = ctx.normal;
    e.metallic = 0.0;
    e.roughness = 1.0;
    e.lighting_mode = VELK_LIGHTING_UNLIT;
    return e;
}
)";

} // namespace

size_t TextureMaterial::get_draw_data_size() const
{
    return sizeof(TextureParams);
}

ReturnValue TextureMaterial::write_draw_data(void* out, size_t size) const
{
    if (auto state = read_state<ITextureVisual>(this)) {
        return set_material<TextureParams>(out, size, [&](auto& p) {
            p.tint = state->tint;
        });
    }
    return ReturnValue::Fail;
}

string_view TextureMaterial::get_eval_src() const
{
    return texture_eval_src;
}

string_view TextureMaterial::get_eval_fn_name() const
{
    return "velk_eval_texture";
}

string_view TextureMaterial::get_vertex_src() const
{
    return rect_material_vertex_src;
}

} // namespace velk::ui::impl
