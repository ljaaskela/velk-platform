#include "gradient_material.h"

#include <velk/api/state.h>
#include <velk-ui/gpu_data.h>

#include <cstring>

namespace velk_ui {

namespace {

const char* gradient_vertex_src = R"(
#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require

layout(buffer_reference, std430) readonly buffer Globals {
    mat4 projection;
    vec4 viewport;
};

struct RectInstance {
    vec2 pos;
    vec2 size;
    vec4 color;
};

layout(buffer_reference, std430) readonly buffer RectInstances {
    RectInstance data[];
};

layout(buffer_reference, std430) readonly buffer DrawData {
    Globals globals;
    RectInstances instances;
    uint texture_id;
    uint instance_count;
    uint _pad0;
    uint _pad1;
    // GradientParams inline after 32-byte header
    vec4 start_color;
    vec4 end_color;
    float angle;
};

layout(push_constant) uniform PC { DrawData root; };

const vec2 kQuad[4] = vec2[4](
    vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(1, 1)
);

layout(location = 0) out vec2 v_local_uv;

void main()
{
    vec2 q = kQuad[gl_VertexIndex];
    RectInstance inst = root.instances.data[gl_InstanceIndex];
    vec2 world_pos = inst.pos + q * inst.size;
    gl_Position = root.globals.projection * vec4(world_pos, 0.0, 1.0);
    v_local_uv = q;
}
)";

const char* gradient_fragment_src = R"(
#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require

// Dummy buffer_reference type to skip over 8-byte pointer fields
layout(buffer_reference, std430) readonly buffer Ptr64 { uint _dummy; };

layout(buffer_reference, std430) readonly buffer DrawData {
    Ptr64 globals;
    Ptr64 instances;
    uint texture_id;
    uint instance_count;
    uint _pad0;
    uint _pad1;
    // GradientParams inline after 32-byte header
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

size_t GradientMaterial::gpu_data_size() const
{
    return sizeof(GradientParams);
}

void GradientMaterial::write_gpu_data(void* out, size_t /*size*/) const
{
    auto state = velk::read_state<IGradient>(this);
    if (!state) return;

    auto& p = *static_cast<GradientParams*>(out);
    p = {};
    p.start_color[0] = state->start_color.r;
    p.start_color[1] = state->start_color.g;
    p.start_color[2] = state->start_color.b;
    p.start_color[3] = state->start_color.a;
    p.end_color[0] = state->end_color.r;
    p.end_color[1] = state->end_color.g;
    p.end_color[2] = state->end_color.b;
    p.end_color[3] = state->end_color.a;
    p.angle = state->angle;
}

} // namespace velk_ui
