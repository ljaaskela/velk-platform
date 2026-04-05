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

constexpr string_view gradient_vertex_src = R"(
#version 450
#include "velk.glsl"
#include "velk-ui.glsl"

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(RectInstanceData)
    vec4 start_color;
    vec4 end_color;
    float angle;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) out vec2 v_local_uv;

void main()
{
    vec2 q = velk_unit_quad(gl_VertexIndex);
    RectInstance inst = root.instance_data.data[gl_InstanceIndex];
    vec2 world_pos = inst.pos + q * inst.size;
    gl_Position = root.global_data.view_projection * vec4(world_pos, 0.0, 1.0);
    v_local_uv = q;
}
)";

constexpr string_view gradient_fragment_src = R"(
#version 450
#include "velk.glsl"

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(Ptr64)
    vec4 start_color;
    vec4 end_color;
    float angle;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) in vec2 v_local_uv;
layout(location = 0) out vec4 frag_color;

void main()
{
    float rad = radians(root.angle);
    vec2 dir = vec2(cos(rad), sin(rad));
    float t = dot(v_local_uv - 0.5, dir) + 0.5;
    t = clamp(t, 0.0, 1.0);
    frag_color = mix(root.start_color, root.end_color, t);
}
)";

} // namespace

constexpr auto gradient_params_size = sizeof(GradientParams);

uint64_t GradientMaterial::get_pipeline_handle(IRenderContext& ctx)
{
    return ensure_pipeline(ctx, gradient_fragment_src, gradient_vertex_src);
}

size_t GradientMaterial::gpu_data_size() const
{
    return gradient_params_size;
}

ReturnValue GradientMaterial::write_gpu_data(void* out, size_t size) const
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

} // namespace velk::ui
