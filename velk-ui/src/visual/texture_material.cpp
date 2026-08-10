#include "texture_material.h"

#include <velk/api/state.h>

#include <cstring>
#include <velk-render/gpu_data.h>
#include <velk-scene/ext/material_shaders.h>

namespace velk::ui::impl {

namespace {

VELK_GPU_STRUCT TextureParams
{
    ::velk::color tint;
};

constexpr string_view texture_eval_src = R"(
struct TextureMaterialData {
    vec4 tint;
};
VELK_MATERIAL(TextureMaterialData)

MaterialEval velk_eval_texture(EvalContext ctx)
{
    TextureMaterialData d = VELK_LOAD_MATERIAL(TextureMaterialData, ctx);
    vec4 sampled = velk_texture(ctx.texture_id, ctx.uv);

    MaterialEval e = velk_default_material_eval();
    e.color = sampled * d.tint;
    e.normal = ctx.normal;
    return e;
}
)";

} // namespace

size_t TextureMaterial::get_draw_data_size() const
{
    return sizeof(TextureParams);
}

ReturnValue TextureMaterial::write_draw_data(void* out, size_t size, ITextureResolver*) const
{
    if (auto state = read_state<ITextureVisual>(this)) {
        return set_material<TextureParams>(out, size, [&](auto& p) {
            p.tint = state->tint;
        });
    }
    return ReturnValue::Fail;
}

string_view TextureMaterial::get_source(string_view role) const
{
    if (role == ::velk::shader_role::kEval)   return texture_eval_src;
    if (role == ::velk::shader_role::kVertex) return rect_material_vertex_src;
    return {};
}

string_view TextureMaterial::get_fn_name(string_view role) const
{
    if (role == ::velk::shader_role::kEval) return "velk_eval_texture";
    return {};
}

} // namespace velk::ui::impl
