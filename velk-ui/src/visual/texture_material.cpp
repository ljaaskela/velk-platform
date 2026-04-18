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

layout(buffer_reference, std430) readonly buffer TextureParams { vec4 tint; };

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(RectInstanceData)
    TextureParams material;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) out vec2 v_uv;

void main()
{
    vec2 q = velk_unit_quad(gl_VertexIndex);
    RectInstance inst = root.instance_data.data[gl_InstanceIndex];
    vec4 local_pos = vec4(inst.pos + q * inst.size, 0.0, 1.0);
    gl_Position = root.global_data.view_projection * inst.world_matrix * local_pos;
    v_uv = q;
}
)";

constexpr string_view texture_fragment_src = R"(
#version 450
#include "velk.glsl"

layout(buffer_reference, std430) readonly buffer TextureParams { vec4 tint; };

layout(buffer_reference, std430) readonly buffer DrawData {
    VELK_DRAW_DATA(OpaquePtr)
    TextureParams material;
};

layout(push_constant) uniform PC { DrawData root; };

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 frag_color;

void main()
{
    frag_color = velk_texture(root.texture_id, v_uv) * root.material.tint;
}
)";

// RT fill snippet. Samples the bindless texture at the hit UV and
// tints by the material's tint. Terminal (no bounce), mirrors
// ImageMaterial::velk_fill_image.
constexpr string_view texture_fill_src = R"(
layout(buffer_reference, std430) readonly buffer TextureMaterialData {
    vec4 tint;
};

BrdfSample velk_fill_texture(FillContext ctx)
{
    TextureMaterialData d = TextureMaterialData(ctx.data_addr);
    vec4 sampled = velk_texture(ctx.texture_id, ctx.uv);
    BrdfSample bs;
    bs.emission = sampled * d.tint;
    bs.throughput = vec3(0.0);
    bs.next_dir = vec3(0.0);
    bs.terminate = true;
    return bs;
}
)";

} // namespace

uint64_t TextureMaterial::get_pipeline_handle(IRenderContext& ctx)
{
    return ensure_pipeline(ctx, texture_fragment_src, texture_vertex_src);
}

string_view TextureMaterial::get_snippet_fn_name() const
{
    return "velk_fill_texture";
}

string_view TextureMaterial::get_snippet_source() const
{
    return texture_fill_src;
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
