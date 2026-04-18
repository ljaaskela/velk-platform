#include "texture_material.h"

#include <velk/api/state.h>
#include <velk-render/gpu_data.h>

#include <cstring>

namespace velk::ui::impl {

namespace {

VELK_GPU_STRUCT TextureParams
{
    ::velk::color tint;
};

constexpr string_view texture_vertex_src = R"(
#version 450
#include "velk.glsl"
#include "velk-ui.glsl"

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(RectInstanceData)
    vec4 tint;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) out vec2 v_uv;
layout(location = 1) flat out uint v_texture_id;
layout(location = 2) out vec4 v_tint;

void main()
{
    vec2 q = velk_unit_quad(gl_VertexIndex);
    RectInstance inst = root.instance_data.data[gl_InstanceIndex];
    vec4 local_pos = vec4(inst.pos + q * inst.size, 0.0, 1.0);
    gl_Position = root.global_data.view_projection * inst.world_matrix * local_pos;
    v_uv = q;
    v_texture_id = root.texture_id;
    v_tint = root.tint;
}
)";

constexpr string_view texture_fragment_src = R"(
#version 450
#extension GL_EXT_nonuniform_qualifier : enable
#include "velk.glsl"

layout(set = 0, binding = 0) uniform sampler2D velk_textures[];

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(Ptr64)
    vec4 tint;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) in vec2 v_uv;
layout(location = 1) flat in uint v_texture_id;
layout(location = 2) in vec4 v_tint;
layout(location = 0) out vec4 frag_color;

void main()
{
    vec4 sampled = texture(velk_textures[nonuniformEXT(v_texture_id)], v_uv);
    frag_color = sampled * v_tint;
}
)";

} // namespace

uint64_t TextureMaterial::get_pipeline_handle(IRenderContext& ctx)
{
    return ensure_pipeline(ctx, texture_fragment_src, texture_vertex_src);
}

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

} // namespace velk::ui::impl
