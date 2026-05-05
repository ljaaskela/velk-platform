#include "shader/shader_compiler.h"

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
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

// Scene shape record: rect / cube / sphere / mesh with a pointer to
// material data. Used by the BVH shape buffer and (duplicated today)
// by RT. shape_kind = 255 is the "complex" sentinel: the shape is a
// triangle mesh, `origin/u_axis` carry the world-space AABB and
// `mesh_data_addr` points at a MeshData record (defined below). The
// field is 0 for non-mesh kinds.
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
    uint shape_kind;  // 0 = rect, 1 = cube, 2 = sphere, 255 = mesh
    uint64_t material_data_addr;
    uint64_t mesh_data_addr;  // shape_kind == 255: MeshData*; otherwise 0
};

layout(buffer_reference, std430) readonly buffer RtShapeList { RtShape data[]; };

// BVH node — used both by the scene-wide TLAS and by the per-mesh
// BLAS that lives in a primitive's MeshStaticData buffer. Inner nodes
// have first_child / child_count; leaves have first_shape / shape_count
// (in the TLAS those index the shape array; in a BLAS they index a
// trailing flat triangle-index array).
struct BvhNode {
    vec4 aabb_min;      // .w padding
    vec4 aabb_max;      // .w padding
    uint first_shape;
    uint shape_count;
    uint first_child;
    uint child_count;
};

layout(buffer_reference, std430) readonly buffer BvhNodeList { BvhNode data[]; };

// Mesh-static metadata, owned by the IMeshPrimitive's persistent
// IDrawData buffer (stable GPU address across frames). Mirrors
// MeshStaticData in scene_collector.h. Layout is followed in the same
// buffer by `blas_node_count` BvhNodes and a flat triangle-index
// array; the shader derives those addresses from blas_node_count.
struct MeshStaticData {
    uint64_t buffer_addr;     // IMeshBuffer GPU address; same buffer holds VBO + IBO
    uint     vbo_offset;      // bytes from buffer_addr to first vertex this primitive uses
    uint     ibo_offset;      // bytes from buffer_addr to first index this primitive uses
    uint     triangle_count;
    uint     vertex_stride;   // bytes per vertex (32 for VelkVertex3D)
    uint     blas_root;       // index of the BLAS root in the trailing BvhNode array
    uint     blas_node_count; // length of the trailing BvhNode array
};

layout(buffer_reference, std430) readonly buffer MeshStaticPtr { MeshStaticData data; };
layout(buffer_reference, std430) readonly buffer BlasNodeList { BvhNode data[]; };
layout(buffer_reference, std430) readonly buffer BlasTriList  { uint     data[]; };

// Per-shape per-frame mesh instance data. Carries the per-element
// transforms plus a pointer to the mesh's static metadata. Allocated
// from the per-frame scratch buffer; SceneBvh patches each cached
// shape's mesh_data_addr to this frame's copy. Mirrors
// MeshInstanceData in scene_collector.h.
struct MeshInstanceData {
    mat4     world;            // mesh-local -> world (for hit attributes)
    mat4     inv_world;        // world -> mesh-local (for transforming the ray)
    uint64_t mesh_static_addr; // -> MeshStaticData; stable across frames
    uint64_t _pad;
};

layout(buffer_reference, std430) readonly buffer MeshInstancePtr { MeshInstanceData data; };

// Index reads from a glTF-style 16-bit-or-32-bit index buffer. We
// always upload 32-bit indices (gltf_decoder.cpp normalises on import),
// so MeshIndices is a uint[] view.
layout(buffer_reference, std430) readonly buffer MeshIndices { uint data[]; };

// Vertex reads as a flat float array. Caller indexes into it using
// `vertex_stride / 4` floats per vertex; the 32-byte VelkVertex3D
// layout is pos[0..2], normal[3..5], uv[6..7].
layout(buffer_reference, std430) readonly buffer MeshVertices { float data[]; };

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

// Per-view FrameGlobals. The renderer writes one FrameGlobals record
// into the per-frame staging buffer per view per frame and pushes its
// GPU address into push-constant slot [0..8) at pass start. Each
// shader's `layout(push_constant)` block must declare a `GlobalData
// globals;` field at offset 0 to receive that address; shaders then
// read view-level state via `globals.X`. Layout matches the FrameGlobals
// struct in gpu_data.h byte-for-byte under scalar layout.
layout(buffer_reference, scalar) readonly buffer GlobalData {
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 viewport;
    vec4 cam_pos;
    uint bvh_root;
    uint bvh_node_count;
    uint bvh_shape_count;
    uint present_counter;
    BvhNodeList bvh_nodes;
    RtShapeList bvh_shapes;
};

// Storage-image arrays for compute imageStore are declared locally
// per-shader (rgba8 -> binding 1, rgba32f -> binding 2, rgba16f ->
// binding 3) rather than here. Putting them in the prelude leaks the
// declarations into fragment shaders that #include velk.glsl, which
// then reflect those bindings into the pipeline layout — but the
// descriptor set layout marks bindings 1..3 as COMPUTE-only, so the
// resulting layout mismatch silently breaks pipeline creation.

// Sample the bindless texture array by id. nonuniformEXT is always
// required because texture ids vary per draw / per shape.
vec4 velk_texture(uint id, vec2 uv)
{
    return texture(velk_textures[nonuniformEXT(id)], uv);
}

// Standard vertex layout used by every visual (2D and 3D). 32-byte
// tight C-style packing (vec3 pos + vec3 normal + vec2 uv) via scalar
// layout — the default std430 rule would pad vec3 to 16 bytes and
// inflate this to 48. Enabled by the Vulkan `scalarBlockLayout`
// feature (see vk_backend). 2D visuals use the unit quad mesh
// (z = 0, normal = +Z); 3D meshes use full xyz positions + normals.
struct VelkVertex3D { vec3 position; vec3 normal; vec2 uv; };
layout(buffer_reference, scalar) readonly buffer VelkVbo3D { VelkVertex3D data[]; };

// Optional TEXCOORD_1 stream: one vec2 per vertex, in a buffer
// parallel to the main VBO. When a primitive has no UV1, DrawData.uv1
// points at a context-owned single-vertex fallback (vec2(0,0)) and
// DrawData.uv1_enabled is 0 so `velk_uv1` reads only index 0. When
// the primitive provides UV1, uv1_enabled is 1 and `velk_uv1` reads
// gl_VertexIndex. Branchless via index multiplication — no shader
// variants.
layout(buffer_reference, scalar) readonly buffer VelkUv1Buffer { vec2 data[]; };

// Vertex-shader helper: fetch current gl_VertexIndex from the VBO.
// Macro (not function) so the readonly memory qualifier on the
// buffer_reference is preserved at the call site, and so velk.glsl
// doesn't reference gl_VertexIndex at namespace scope (would break
// fragment shaders that also include this file).
#define velk_vertex3d(root) ((root).vbo.data[gl_VertexIndex])

// Vertex-shader helper: fetch the current vertex's UV1. Uses
// uv1_enabled as a branchless index multiplier — 0 forces index 0 so
// the single-vertex fallback buffer is always in range; 1 reads the
// per-vertex stream at gl_VertexIndex.
#define velk_uv1(root) ((root).uv1.data[(root).uv1_enabled * gl_VertexIndex])

// Standard DrawData header fields. Use inside a buffer_reference block:
//   layout(buffer_reference, std430) readonly buffer DrawData {
//       VELK_DRAW_DATA(ElementInstanceData, VelkVbo3D)
//       vec4 my_material_param;  // optional material fields follow
//   };
// `VboType` is a `buffer_reference`-typed handle to the vertex buffer
// (typically `VelkVbo3D`, the unified scalar-packed vertex layout).
// The 48-byte header keeps everything 16-byte aligned for std430.
#define VELK_DRAW_DATA(InstancesType, VboType) \
    OpaquePtr _reserved0;                      \
    InstancesType instance_data;               \
    uint texture_id;                           \
    uint instance_count;                       \
    VboType vbo;                               \
    VelkUv1Buffer uv1;                         \
    uint uv1_enabled;                          \
    uint _pad_uv1;
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
