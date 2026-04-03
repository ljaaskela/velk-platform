#include "gradient_material.h"

namespace velk_ui {

namespace {

const char* gradient_vertex_src = R"(
#version 450
#include "velk_common.glsl"

layout(location = 0) in vec4 inst_rect;
layout(location = 1) in vec4 inst_color;

layout(location = 0) out vec2 v_local_uv;

void main()
{
    vec2 pos = vec2(float(VERTEX_INDEX & 1), float(VERTEX_INDEX >> 1));
    vec2 world_pos = inst_rect.xy + pos * inst_rect.zw;
    gl_Position = u_projection * vec4(world_pos, 0.0, 1.0);
    v_local_uv = pos;
}
)";

const char* gradient_fragment_src = R"(
#version 450

layout(location = 0) in vec2 v_local_uv;
layout(location = 0) out vec4 frag_color;

layout(std140, binding = 1) uniform MaterialParams {
    vec4 start_color;
    vec4 end_color;
    float angle;
};

void main()
{
    float rad = radians(angle);
    vec2 dir = vec2(cos(rad), sin(rad));
    float t = dot(v_local_uv - 0.5, dir) + 0.5;
    t = clamp(t, 0.0, 1.0);
    frag_color = mix(start_color, end_color, t);
}
)";

} // namespace

uint64_t GradientMaterial::get_pipeline_handle(IRenderContext& ctx)
{
    if (!shader_mat_) {
        auto obj = ctx.create_shader_material(gradient_fragment_src, gradient_vertex_src);
        if (obj) {
            shader_mat_ = interface_pointer_cast<IMaterial>(obj);
        }
    }
    return shader_mat_ ? shader_mat_->get_pipeline_handle(ctx) : 0;
}

} // namespace velk_ui
