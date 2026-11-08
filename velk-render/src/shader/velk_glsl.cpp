#include "shader/velk_glsl.h"

namespace velk {

// Framework-level shader declarations registered automatically into every
// RenderContext as the "velk.glsl" virtual include. Exposed so the shader
// cache can include its content in the cache key hash.
// NOTE: the GLSL below is registered as the "velk.glsl" virtual include and its
// bytes are hashed into every shader's cache key, so keep prose in C++ comments
// BETWEEN the chunks rather than inside the string literals. Comments inside
// would ship in the binary, be re-lexed on every cold compile, count against
// MSVC's 16 KB raw-string limit, and invalidate the whole shader cache whenever
// they are edited. Adjacent string literals concatenate, so the GLSL the
// compiler sees is one continuous file.
const string_view kVelkGlsl = R"(
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require
)"
// Scene shape, in its record's local space: rect / cube / sphere / mesh with a
// pointer to material data. Used by the BVH shape buffer and by RT primary rays.
// shape_kind = 255 is the "complex" sentinel: the shape is a triangle mesh,
// `origin/u_axis` carry its local AABB and `mesh_static_base` indexes the
// mesh-static buffer (set = 1 slot 11). The field is 0 for non-mesh kinds.
// `primary_record` is the shape's record for RT primary rays, which walk a flat
// list and so have no record of their own to come from.
R"(
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
    uint material_base;       // word index of this shape's record in the material arena
    uint primary_record;      // index into velk_records; RT primary list only
    uint mesh_static_base;    // shape_kind == 255: index into velk_mesh_static_records
    uint _pad1;
};

)"
// BVH node, used both by the scene-wide TLAS and by the per-mesh BLAS that
// lives in a primitive's MeshStaticData buffer. Inner nodes have first_child /
// child_count; leaves have first_shape / shape_count (in the TLAS those index
// the shape array; in a BLAS they index a trailing flat triangle-index array).
R"(
struct BvhNode {
    vec4 aabb_min;      // .w padding
    vec4 aabb_max;      // .w padding
    uint first_shape;
    uint shape_count;
    uint first_child;
    uint child_count;
};

// child_count of a node that refers to a partition. Its first_child is the
// partition's root, and its origin is in the fields read by velk_partition_origin.
#define VELK_PARTITION_REF 0xFFFFFFFFu
#define velk_partition_origin(node) \
    vec3((node).aabb_min.w, (node).aabb_max.w, uintBitsToFloat((node).first_shape))

)"
// Mesh-static metadata, owned by the IMeshPrimitive as a persistent region of
// the shared mesh-static arena (set = 1 slot 11), so its element index is
// stable across frames. Mirrors MeshStaticData in gpu_data.h. The BLAS it
// describes lives in its own arenas (slots 12 / 13), reached by blas_node_base
// / cluster_word_base rather than by trailing this record.
R"(
struct MeshStaticData {
    uint     vbo_base;        // word base of this primitive's first vertex in velk_mesh_words
    uint     flags;           // VELK_MESH_SCATTERED: wide rays may count parts of it as a medium
    uint     triangle_count;
    uint     vertex_stride;   // bytes per vertex, from the primitive (48 for VelkVertex3D)
    uint     blas_root;       // root index within this primitive's BLAS node run
    uint     blas_node_count; // length of this primitive's BLAS node run; 0 = no BLAS
    uint     blas_node_base;  // element base of the node run in velk_blas_nodes
    uint     cluster_word_base; // element base of this primitive's clusters in velk_cluster_words
};

)"
// Places shapes in the world (set = 1 slot 10). Rays reach a shape through
// velk_local_ray. The buffer itself is declared by the compute preludes that
// trace. Mirrors RtRecord in gpu_data.h.
R"(
struct RtRecord {
    vec4 inv_row0;     // xyz: world -> local rotation and scale, by rows. w: world position of local origin
    vec4 inv_row1;
    vec4 inv_row2;
    uint first_shape;  // index into velk_bvh_shapes
    uint shape_count;
    uint tree_root;    // index into velk_blas_nodes, or VELK_NO_TREE to test each shape
    uint flags;        // 1: rays follow the scene's ray motion here. 2: wide rays count parts of its meshes as a medium
};

#define VELK_NO_TREE 0xFFFFFFFFu

)"
// mesh_static_base value for a mesh whose static record is not resolvable yet
// (geometry not uploaded, no BLAS built). Mirrors kInvalidMeshStaticBase in
// gpu_data.h.
R"(
#define VELK_INVALID_MESH_STATIC 0xFFFFFFFFu
#define VELK_MESH_SCATTERED 1u
#define VELK_MESH_DOUBLE_SIDED 2u
)"
// Every mesh's VBO + IBO bytes as raw 32-bit words (set = 1 slot 14). Both
// paths read geometry from here: RT walks it by triangle, raster fetches the
// current vertex. Declared here rather than per-shader since every stage can
// see set = 1.
R"(
layout(set = 1, binding = 14, std430) readonly buffer VelkMeshWords { uint data[]; } velk_mesh_words;
)"
// Mesh geometry accessors over that arena, for the RT side, which reaches a
// primitive's runs through its MeshStaticData record.
//
// Vertices: a flat float array reached by word. Caller indexes using
// `vertex_stride / 4` words per vertex and reads pos[0..2], normal[3..5],
// uv[6..7]. Those sit at the same offsets in VelkVertex3D, whose trailing
// tangent RT does not read, so both paths share one layout. The words are
// float bits, and uintBitsToFloat is a reinterpret, not a conversion.
//   float x = velk_mesh_vertex(st, o0 + 0u);
R"(
#define velk_mesh_vertex(st, i) uintBitsToFloat(velk_mesh_words.data[(st).vbo_base + (i)])
)"
// Framework-level bindless texture array. Every pipeline in the engine shares
// this descriptor set binding; individual shaders reference a texture by index
// rather than declaring their own samplers.
R"(
layout(set = 0, binding = 0) uniform sampler2D velk_textures[];
)"
// Per-view FrameGlobals as a value type. `GlobalData` is the element of the
// set = 1 globals buffer (velk_globals below); shaders reach it by index
// (`velk_globals.data[globals_base]`) via velk_global_data(root). Layout
// matches the FrameGlobals struct in gpu_data.h byte-for-byte under scalar
// layout.
R"(
struct GlobalData {
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 viewport;
    vec4 cam_pos;
    uint bvh_root;
    uint bvh_node_count;
    uint present_counter;
    uint _pad_globals1;
    mat4 prev_view_projection;
    vec3 cam_moved;          // how far the camera moved since the previous frame
    uint _pad_globals2;
};

// Shaders position things relative to the camera, so that large world coordinates
// cancel before they lose precision. view_projection and G-buffer positions are in
// that space; cam_pos is the camera's world position.
vec3 velk_rel_position(mat4 world, vec3 local, GlobalData globals)
{
    return mat3(world) * local + (world[3].xyz - globals.cam_pos.xyz);
}

)"
// Frame-invariant per-view globals, bound at set = 1 slot 2 and read by index
// in every stage (graphics via velk_global_data(root), compute via the
// per-source VELK_GLOBALS macro). Declared once here: set = 1 is visible to
// graphics as well as compute, so this does not leak a compute-only descriptor
// into raster pipelines.
//
// Storage-image arrays for compute imageStore are declared locally per-shader
// (rgba8 -> binding 1, rgba32f -> binding 2, rgba16f -> binding 3) rather than
// here. Putting them in the prelude leaks the declarations into fragment
// shaders that include velk.glsl, which then reflect those bindings into the
// pipeline layout; the descriptor set layout marks bindings 1..3 as
// COMPUTE-only, so the resulting mismatch silently breaks pipeline creation.
R"(
layout(set = 1, binding = 2, scalar) readonly buffer VelkGlobals { GlobalData data[]; } velk_globals;
)"
// Sample the bindless texture array by id. nonuniformEXT is always required
// because texture ids vary per draw / per shape.
R"(
#ifdef VELK_COMPUTE
// Compute has no implicit derivatives. A pass that knows a pixel's footprint sets how each
// uv set changes per pixel; otherwise, as for rays, textures are read at their finest level.
vec2 velk_uv_dx[2] = vec2[2](vec2(0.0), vec2(0.0));
vec2 velk_uv_dy[2] = vec2[2](vec2(0.0), vec2(0.0));
uint velk_uv_set = 0u;

vec4 velk_texture(uint id, vec2 uv)
{
    return textureGrad(velk_textures[nonuniformEXT(id)], uv, velk_uv_dx[velk_uv_set], velk_uv_dy[velk_uv_set]);
}

// Stands in for fwidth of the first uv set, with a fixed estimate where the footprint is unknown.
vec2 velk_uv_fwidth()
{
    vec2 width = abs(velk_uv_dx[0]) + abs(velk_uv_dy[0]);
    return width.x + width.y > 0.0 ? width : vec2(1.0 / 32.0);
}
#else
vec4 velk_texture(uint id, vec2 uv)
{
    return texture(velk_textures[nonuniformEXT(id)], uv);
}
#endif
)"
// Standard vertex layout used by every visual (2D and 3D). 48-byte tight
// C-style packing (vec3 pos + vec3 normal + vec2 uv + vec4 tangent) via scalar
// layout; the default std430 rule would pad the vec3s to 16 bytes. Enabled by
// the Vulkan `scalarBlockLayout` feature (see vk_backend). 2D visuals use the
// unit quad mesh (z = 0, normal = +Z); 3D meshes use full xyz.
// tangent = glTF TANGENT (xyz world-space dir + w handedness), synthesized for
// procedural meshes that carry none.
//
// VELK_VERTEX3D_WORDS is 12 floats (pos 0..2, normal 3..5, uv 6..7, tangent
// 8..11). The RT side reads only the first 8 and so is indifferent to the
// tangent, which is why it can share this layout.
R"(
struct VelkVertex3D { vec3 position; vec3 normal; vec2 uv; vec4 tangent; };
#define VELK_VERTEX3D_WORDS 12u
)"
// Vertex-shader helper: fetch the current gl_VertexIndex from the mesh-word
// arena at the draw's vertex base. velk_vertex3d is a macro, not a function,
// so velk.glsl does not reference gl_VertexIndex at namespace scope (which
// would break fragment shaders that also include this file). The words are
// float bits; uintBitsToFloat is a reinterpret, not a conversion.
R"(
float velk_vertex_word(uint base, uint w)
{
    return uintBitsToFloat(velk_mesh_words.data[base + w]);
}

VelkVertex3D velk_unpack_vertex3d(uint base)
{
    VelkVertex3D v;
    v.position = vec3(velk_vertex_word(base, 0u), velk_vertex_word(base, 1u),
                      velk_vertex_word(base, 2u));
    v.normal   = vec3(velk_vertex_word(base, 3u), velk_vertex_word(base, 4u),
                      velk_vertex_word(base, 5u));
    v.uv       = vec2(velk_vertex_word(base, 6u), velk_vertex_word(base, 7u));
    v.tangent  = vec4(velk_vertex_word(base, 8u), velk_vertex_word(base, 9u),
                      velk_vertex_word(base, 10u), velk_vertex_word(base, 11u));
    return v;
}

)"
// Every accessor below takes the draw handle declared by VELK_DRAW_DATA(Name)
// and reaches its record through velk_draw() (defined at the bottom of this
// file, next to the record type).
//
// velk_uv1 fetches the current vertex's UV1, a vec2 stream parallel to the
// main VBO. When a primitive has no UV1, `uv1_base` points at a context-owned
// single-vertex fallback (vec2(0,0)) and `uv1_enabled` is 0, so this reads
// vertex 0. Branchless via index multiplication, so there are no variants.
R"(
#define velk_vertex3d(root) \
    velk_unpack_vertex3d(velk_draw(root).vbo_base + uint(gl_VertexIndex) * VELK_VERTEX3D_WORDS)

#define velk_uv1(root)                                                           \
    vec2(velk_vertex_word(velk_draw(root).uv1_base,                                \
                          velk_draw(root).uv1_enabled * uint(gl_VertexIndex) * 2u), \
         velk_vertex_word(velk_draw(root).uv1_base,                                \
                          velk_draw(root).uv1_enabled * uint(gl_VertexIndex) * 2u + 1u))

)"
// Per-view FrameGlobals for this draw. The header's `globals_base` is an index
// into the set = 1 globals buffer; the macro hides the field so callers stay
// decoupled from the header layout.
//   GlobalData globals = velk_global_data(root);
R"(
#define velk_global_data(root) (velk_globals.data[velk_draw(root).globals_base])
)"
// Per-shader typed view of the shared instance arena (set = 1 slot 3). Declare
// once at file scope with the shader's instance struct:
//   VELK_INSTANCES(ElementInstance)
//   ElementInstance inst = velk_instance(root);
// The buffer holds every batch's instance data; each draw reads its own run at
// `instances_base`, by gl_InstanceIndex or, for a culled draw, by the visible
// list in the culling arena. Different shaders bind the same slot with their own
// element type (the buffer is raw bytes; each pipeline interprets its own run).
// velk_instance is vertex-stage only.
R"(
#define VELK_INSTANCES(InstancesType) \
    layout(set = 1, binding = 3, scalar) readonly buffer VelkInstances { InstancesType data[]; } velk_instances;

#define VELK_NO_VISIBLE_LIST 0xFFFFFFFFu

#define velk_instance_slot(root)                                                     \
    (velk_draw(root).visible_base == VELK_NO_VISIBLE_LIST                            \
         ? uint(gl_InstanceIndex)                                                    \
         : velk_culling.data[velk_draw(root).visible_base + uint(gl_InstanceIndex)])

#define velk_instance(root) (velk_instances.data[velk_draw(root).instances_base + velk_instance_slot(root)])

// How far this draw instance's surface may be from the full mesh, in world units.
#define velk_lod_error(root)                                                         \
    (velk_draw(root).lod_error_base == VELK_NO_VISIBLE_LIST                          \
         ? 0.0                                                                       \
         : uintBitsToFloat(velk_culling.data[velk_draw(root).lod_error_base + uint(gl_InstanceIndex)]))
)"
// Per-pipeline typed view of the shared material arena (set = 1 slot 4).
// Declare once at file scope with this pipeline's material struct:
//   VELK_MATERIAL(CheckerParams)
//   CheckerParams m = velk_material(root);
// The buffer holds every material's draw data; this draw reads its own record
// at `material_base`. Each graphics pipeline binds the same slot with its own
// material struct (the arena is raw bytes; each material's region is aligned to
// its record size so the element index lands on it). Raster only: the RT /
// deferred compute path serves many material types from one shader, so its
// composer replaces the VELK_MATERIAL line with a generated velk_unpack_<T>
// that rebuilds the struct from the arena's raw words.
R"(
#define VELK_MATERIAL(MaterialType) \
    layout(set = 1, binding = 4, std430) readonly buffer VelkMaterials { MaterialType data[]; } velk_materials;

#define velk_material(root) (velk_materials.data[velk_draw(root).material_base])
)"
// Mesh intersector accessors, for the compute preludes that trace meshes.
// Unlike materials and instances the element type never varies, so there is no
// declaration macro; those preludes declare velk_mesh_static_records themselves.
//   MeshStaticData st = velk_mesh_static(shape);
// Guard with `shape.mesh_static_base != VELK_INVALID_MESH_STATIC`.
R"(
#define velk_mesh_static(shape) (velk_mesh_static_records.data[(shape).mesh_static_base])
)"
// Per-draw header: 48 bytes of indices and counts, one record per batch in the
// set = 1 draw-data arena (slot 15). Mirrors DrawDataHeader in gpu_data.h.
// Shader bodies reach these through accessors that hide the layout rather than
// naming the fields: velk_global_data(root), velk_instance(root),
// velk_material(root), velk_vertex3d(root), velk_uv1(root). The two fields with
// no accessor of their own, texture_id and material_base, are read by the
// composed raster drivers via velk_draw(root).
R"(
struct VelkDrawData {
    uint globals_base;
    uint instances_base;
    uint texture_id;
    uint visible_base;
    uint vbo_base;
    uint uv1_base;
    uint uv1_enabled;
    uint material_base;
    uint lod_error_base;
    uint triangle_bits;
    uint material_id;
    uint material_words;
    uint index_base;
    uint cluster_first_base;
    uint _pad_draw0;
    uint _pad_draw1;
};

layout(set = 1, binding = 15, std430) readonly buffer VelkDrawDataBuf { VelkDrawData data[]; } velk_draw_data;
layout(set = 1, binding = 16, std430) readonly buffer VelkCulling { uint data[]; } velk_culling;
)"
// Declares a raster shader's push constant and names it. Put it at file scope,
// once per shader (a shader may have only one push-constant block):
//   VELK_DRAW_DATA(root)
// after which `root` is what every accessor above takes. A raster pipeline is
// handed nothing else; everything hangs off `root`.
//
// The accessors require only that their argument has a `draw_base` member, not
// that it IS this block, so a future GPU-driven path can select a record per
// draw (from gl_DrawID, say) by passing a local struct instead, leaving every
// shader body unchanged.
//
// velk_draw is the record itself. Shaders normally go through the field
// accessors above; use it only for a header field that has no accessor, as the
// composed raster drivers do for texture_id / material_base:
//   uint tex = velk_draw(root).texture_id;
R"(
#define VELK_DRAW_DATA(Name) \
    layout(push_constant, std430) uniform VelkPC { uint draw_base; } Name;

#define velk_draw(r) (velk_draw_data.data[(r).draw_base])
)"
// The varyings `element_vertex_src` writes and every composed raster fragment
// driver reads. One declaration each side, so adding a varying is a single edit
// here rather than one per shader. A fragment may ignore any of them; the
// vertex stage always writes all ten.
//
//   VELK_VARYINGS_OUT   // in a vertex shader
//   VELK_VARYINGS_IN    // in a fragment shader
//
// Shaders that supply BOTH stages themselves may declare their own set instead;
// these describe the shared contract, not a requirement. primitive_shaders.h is
// the one such pair in the tree.
R"(
#define VELK_VARYINGS_OUT                           \
    layout(location = 0) out vec4 v_color;            \
    layout(location = 1) out vec2 v_local_uv;         \
    layout(location = 2) flat out vec2 v_size;        \
    layout(location = 3) out vec3 v_rel_pos;          \
    layout(location = 4) out vec3 v_world_normal;     \
    layout(location = 5) flat out uint v_shape_param; \
    layout(location = 6) out vec2 v_uv1;              \
    layout(location = 7) out vec4 v_world_tangent;    \
    layout(location = 8) flat out float v_lod_error;  \
    layout(location = 9) flat out uint v_instance;

#define VELK_VARYINGS_IN                             \
    layout(location = 0) in vec4 v_color;            \
    layout(location = 1) in vec2 v_local_uv;         \
    layout(location = 2) flat in vec2 v_size;        \
    layout(location = 3) in vec3 v_rel_pos;          \
    layout(location = 4) in vec3 v_world_normal;     \
    layout(location = 5) flat in uint v_shape_param; \
    layout(location = 6) in vec2 v_uv1;              \
    layout(location = 7) in vec4 v_world_tangent;    \
    layout(location = 8) flat in float v_lod_error;  \
    layout(location = 9) flat in uint v_instance;

)"
// VELK_FRAG_OUT is the single-target colour output, for forward and any full
// fragment shader. It takes the name so the identifier a shader assigns to is
// one it declared:
//   VELK_FRAG_OUT(frag_color)
R"(
#define VELK_FRAG_OUT(Name) layout(location = 0) out vec4 Name;
)";

} // namespace velk
