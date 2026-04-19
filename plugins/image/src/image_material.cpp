#include "image_material.h"

#include <velk/api/state.h>
#include <velk-render/gpu_data.h>
#include <velk-ui/ext/material_shaders.h>

#include <cstring>

namespace velk::ui::impl {

namespace {

VELK_GPU_STRUCT ImageParams
{
    ::velk::color tint;
};

constexpr string_view image_eval_src = R"(
layout(buffer_reference, std430) readonly buffer ImageMaterialData {
    vec4 tint;
};

MaterialEval velk_eval_image(EvalContext ctx)
{
    ImageMaterialData d = ImageMaterialData(ctx.data_addr);
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

size_t ImageMaterial::get_draw_data_size() const
{
    return sizeof(ImageParams);
}

ReturnValue ImageMaterial::write_draw_data(void* out, size_t size) const
{
    if (auto state = read_state<IImageMaterial>(this)) {
        return set_material<ImageParams>(out, size, [&](auto& p) {
            p.tint = state->tint;
        });
    }
    return ReturnValue::Fail;
}

string_view ImageMaterial::get_eval_src() const
{
    return image_eval_src;
}

string_view ImageMaterial::get_eval_fn_name() const
{
    return "velk_eval_image";
}

string_view ImageMaterial::get_vertex_src() const
{
    return rect_material_vertex_src;
}

} // namespace velk::ui::impl
