#include "shader_compiler.h"

#include <velk/api/perf.h>
#include <velk/api/velk.h>

#include <cstring>
#include <shaderc/shaderc.hpp>
#include <string>

namespace velk {

// Framework-level shader declarations registered automatically into every
// RenderContext as the "velk.glsl" virtual include. Exposed so the shader
// cache can include its content in the cache key hash.
const char* kVelkGlsl = R"(
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

// Scene shape record: rect / cube / sphere with a pointer to material
// data. Used by the BVH shape buffer and (duplicated today) by RT.
struct RtShape {
    vec4 origin;
    vec4 u_axis;
    vec4 v_axis;
    vec4 w_axis;
    vec4 color;
    vec4 params;
    uint material_id;
    uint texture_id;
    uint shape_param;
    uint shape_kind;  // 0 = rect, 1 = cube, 2 = sphere
    uint64_t material_data_addr;
    uint64_t _tail_pad;
};

layout(buffer_reference, std430) readonly buffer RtShapeList { RtShape data[]; };

// Scene-wide BVH node. Built CPU-side from the element tree in
// scene_collector; referenced from GlobalData so every shader sees the
// same acceleration structure.
struct BvhNode {
    vec4 aabb_min;      // .w padding
    vec4 aabb_max;      // .w padding
    uint first_shape;   // index into the scene's RtShape buffer
    uint shape_count;
    uint first_child;   // index of the first child BvhNode
    uint child_count;
};

layout(buffer_reference, std430) readonly buffer BvhNodeList { BvhNode data[]; };

// Frame globals: projection matrix, its inverse, viewport, BVH metadata.
layout(buffer_reference, std430) readonly buffer GlobalData {
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 viewport;          // width, height, 1/width, 1/height
    vec4 cam_pos;           // xyz = world-space camera position, w = _
    uint bvh_root;
    uint bvh_node_count;
    uint bvh_shape_count;
    uint _pad0;
    BvhNodeList bvh_nodes;
    RtShapeList bvh_shapes;
};

// Generic 8-byte buffer_reference placeholder. Use it wherever the
// shader needs to preserve the layout of a typed pointer field without
// caring about the target's contents (e.g. fragment shaders that skip
// the instance_data slot, or vertex shaders that don't dereference
// typed sub-pointers inside a material block).
layout(buffer_reference, std430) readonly buffer OpaquePtr { uint _dummy; };

// Framework-level bindless texture array. Every pipeline in the engine
// shares this descriptor set binding; individual shaders reference a
// texture by index rather than declaring their own samplers.
layout(set = 0, binding = 0) uniform sampler2D velk_textures[];

// Sample the bindless texture array by id. nonuniformEXT is always
// required because texture ids vary per draw / per shape.
vec4 velk_texture(uint id, vec2 uv)
{
    return texture(velk_textures[nonuniformEXT(id)], uv);
}

// Standard 2D vertex layout shared by all UI visuals (rect, rounded
// rect, text glyphs, image, texture, env). Matches the unit quad mesh
// shipped by IMeshBuilder::get_unit_quad().
//
// IBO is bound on the host side via vkCmdBindIndexBuffer + dispatched
// with vkCmdDrawIndexed, so the vertex shader's gl_VertexIndex is the
// post-fetch index into the VBO directly (no shader-side IBO lookup).
struct VelkVertex2D { vec2 position; };
layout(buffer_reference, std430) readonly buffer VelkVbo2D { VelkVertex2D data[]; };

// Vertex-shader helper: position from the VBO at the current
// gl_VertexIndex. Macro (not function) so the readonly memory
// qualifier on the buffer_reference is preserved at the call site,
// and so velk.glsl doesn't reference gl_VertexIndex at namespace
// scope (would break fragment shaders that also include this file).
#define velk_vertex_pos2d(root) ((root).vbo.data[gl_VertexIndex].position)

// Standard DrawData header fields. Use inside a buffer_reference block:
//   layout(buffer_reference, std430) readonly buffer DrawData {
//       VELK_DRAW_DATA(RectInstanceData, MyVbo)
//       vec4 my_material_param;  // optional material fields follow
//   };
// `VboType` is a `buffer_reference`-typed handle whose target struct
// matches the material's vertex layout (e.g. `VelkVbo2D` for the unit
// quad). The 32-byte header keeps everything 8-byte aligned for std430.
#define VELK_DRAW_DATA(InstancesType, VboType) \
    GlobalData global_data;                    \
    InstancesType instance_data;               \
    uint texture_id;                           \
    uint instance_count;                       \
    VboType vbo;
)";

namespace {

class VelkIncluder : public shaderc::CompileOptions::IncluderInterface
{
public:
    explicit VelkIncluder(const ShaderIncludeMap* user_includes) : user_includes_(user_includes) {}

    shaderc_include_result* GetInclude(const char* requested_source, shaderc_include_type, const char*,
                                       size_t) override
    {
        auto* result = new shaderc_include_result{};

        if (user_includes_) {
            auto it = user_includes_->find(requested_source);
            if (it != user_includes_->end()) {
                // Store the name so it outlives this call
                cached_name_ = it->first;
                result->source_name = cached_name_.c_str();
                result->source_name_length = cached_name_.size();
                result->content = it->second.c_str();
                result->content_length = it->second.size();
            } else {
                set_error(result, requested_source);
            }
        } else {
            set_error(result, requested_source);
        }

        return result;
    }

    void ReleaseInclude(shaderc_include_result* data) override { delete data; }

private:
    void set_error(shaderc_include_result* result, const char* name)
    {
        error_msg_ = string("unknown include: ") + name;
        result->source_name = "";
        result->source_name_length = 0;
        result->content = error_msg_.c_str();
        result->content_length = error_msg_.size();
    }

    const ShaderIncludeMap* user_includes_;
    string cached_name_;
    string error_msg_;
};

} // namespace

vector<uint32_t> compile_glsl_to_spirv(string_view source, ShaderStage stage,
                                       const ShaderIncludeMap* user_includes)
{
    VELK_PERF_SCOPE("vk.compile_glsl_to_spirv");
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;

    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    options.SetTargetSpirv(shaderc_spirv_version_1_5);
    options.SetGenerateDebugInfo();
    options.SetIncluder(std::make_unique<VelkIncluder>(user_includes));

    shaderc_shader_kind kind = shaderc_fragment_shader;
    const char* filename = "fragment.glsl";
    switch (stage) {
    case ShaderStage::Vertex:
        kind = shaderc_vertex_shader;
        filename = "vertex.glsl";
        break;
    case ShaderStage::Fragment:
        kind = shaderc_fragment_shader;
        filename = "fragment.glsl";
        break;
    case ShaderStage::Compute:
        kind = shaderc_compute_shader;
        filename = "compute.glsl";
        break;
    }

    auto result = compiler.CompileGlslToSpv(source.data(), kind, filename, options);

    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        VELK_LOG(E, "Shader compilation failed: %s", result.GetErrorMessage().c_str());
        return {};
    }

    return {result.cbegin(), result.cend()};
}

} // namespace velk
