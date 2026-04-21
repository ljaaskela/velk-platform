#include "gradient_material.h"

#include <velk/api/state.h>

#include <cstring>
#include <velk-render/gpu_data.h>

namespace velk::ui {

namespace {

VELK_GPU_STRUCT GradientParams
{
    ::velk::color start_color;
    ::velk::color end_color;
    float angle;
};

// Eval body: linear gradient across the local rect UV, driven by
// `angle` in degrees. Shared across forward / deferred / RT; the
// framework wraps this into the appropriate per-path driver.
constexpr string_view gradient_eval_src = R"(
layout(buffer_reference, std430) readonly buffer GradientMaterialData {
    vec4 start_color;
    vec4 end_color;
    vec4 angle_pad; // x = angle in degrees; yzw unused
};

MaterialEval velk_eval_gradient(EvalContext ctx)
{
    GradientMaterialData d = GradientMaterialData(ctx.data_addr);
    float rad = radians(d.angle_pad.x);
    vec2 dir = vec2(cos(rad), sin(rad));
    float t = clamp(dot(ctx.uv - 0.5, dir) + 0.5, 0.0, 1.0);

    MaterialEval e = velk_default_material_eval();
    e.color = mix(d.start_color, d.end_color, t);
    e.normal = ctx.normal;
    return e;
}
)";

} // namespace

constexpr auto gradient_params_size = sizeof(GradientParams);

size_t GradientMaterial::get_draw_data_size() const
{
    return gradient_params_size;
}

ReturnValue GradientMaterial::write_draw_data(void* out, size_t size, ITextureResolver*) const
{
    if (auto state = read_state<IGradient>(this)) {
        return set_material<GradientParams>(out, size, [&](auto& p) {
            p.start_color = state->start_color;
            p.end_color = state->end_color;
            p.angle = state->angle;
        });
    }
    return ReturnValue::Fail;
}

string_view GradientMaterial::get_eval_src() const
{
    return gradient_eval_src;
}

string_view GradientMaterial::get_eval_fn_name() const
{
    return "velk_eval_gradient";
}


} // namespace velk::ui
