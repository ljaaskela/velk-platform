#include "image_material.h"

#include <velk/api/state.h>
#include <velk-render/gpu_data.h>
#include <velk-scene/ext/material_shaders.h>

#include <cstring>

namespace velk::ui::impl {

namespace {

VELK_GPU_STRUCT ImageParams
{
    ::velk::color tint;
};

constexpr string_view image_eval_src = R"(
struct ImageMaterialData {
    vec4 tint;
};
VELK_MATERIAL_BUFFER(ImageMaterialData, ImageMaterialRef)

MaterialEval velk_eval_image(EvalContext ctx)
{
    ImageMaterialData d = VELK_LOAD_MATERIAL(ImageMaterialData, ImageMaterialRef, ctx);
    vec4 sampled = velk_texture(ctx.texture_id, ctx.uv);

    MaterialEval e = velk_default_material_eval();
    e.color = sampled * d.tint;
    e.normal = ctx.normal;
    return e;
}
)";

} // namespace

size_t ImageMaterial::get_draw_data_size() const
{
    return sizeof(ImageParams);
}

ReturnValue ImageMaterial::write_draw_data(void* out, size_t size, ITextureResolver*) const
{
    if (auto state = read_state<IImageMaterial>(this)) {
        return set_material<ImageParams>(out, size, [&](auto& p) {
            p.tint = state->tint;
        });
    }
    return ReturnValue::Fail;
}

string_view ImageMaterial::get_source(string_view role) const
{
    if (role == ::velk::shader_role::kEval) return image_eval_src;
    return Base::get_source(role);
}

string_view ImageMaterial::get_fn_name(string_view role) const
{
    if (role == ::velk::shader_role::kEval) return "velk_eval_image";
    return {};
}

} // namespace velk::ui::impl
