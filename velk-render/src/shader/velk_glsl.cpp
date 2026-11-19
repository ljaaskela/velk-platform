#include "shader/velk_glsl.h"

#include <velk-render/frame/cull_layout.h>

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
// The words of the culling's lists and kept cuts, as cull_layout.h names them.
VELK_CULL_LAYOUT_GLSL
// An analytic shape, in its instance's own space: what rays test a visual drawn by its
// own pass as (velk_instance_shape), and what intersect snippets are given.
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
    uint material_base;       // word index of its material's record in the material arena
    uint _pad0;
    uint _pad1;
    uint _pad2;
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

// A node of a mesh's tree, in one cache line: the boxes of its two children, packed as left
// min, left max, right min, right max, then in refs the left and right child and each one's
// surface, its packed normal. A child with
// VELK_BLAS_LEAF set is a word offset to a leaf's first triangle word, its counts and its
// positions.
struct VelkMeshNode {
    vec4 a;
    vec4 b;
    vec4 c;
    uvec4 refs;
};
#define VELK_BLAS_LEAF 0x80000000u
// On a child of a mesh's tree without VELK_BLAS_LEAF: one cluster of its level of detail
// hierarchy, with the word of its header in velk_cluster_words over four (kBlasClusterRef).
// Without that either: a node of the tree over those clusters, with its index in
// velk_mesh_nodes (kBlasUpperRef). Anything else is a node of the tree being walked.
#define VELK_BLAS_CLUSTER 0x40000000u
#define VELK_BLAS_UPPER 0x20000000u

// child_count of a node of the scene's tree that stands for a partition with its
// instances: the far instance splat goes no further.
#define VELK_PARTITION_REF 0xFFFFFFFFu

)"
// Mesh-static metadata, owned by the IMeshPrimitive as a persistent region of
// the shared mesh-static arena (set = 1 slot 11), so its element index is
// stable across frames. Mirrors MeshStaticData in gpu_data.h. The BLAS it
// describes lives in its own arenas (slots 12 / 13), reached by blas_node_base
// / cluster_word_base rather than by trailing this record.
R"(
struct MeshStaticData {
    uint     vbo_base;        // word base of this primitive's first vertex in velk_mesh_words
    uint     flags;           // VELK_MESH_DOUBLE_SIDED, VELK_MESH_DEFORMING
    uint     triangle_count;
    uint     vertex_stride;   // bytes per vertex, from the primitive (48 for VelkVertex3D)
    uint     blas_root;       // root index within this primitive's BLAS node run
    uint     blas_node_count; // length of this primitive's BLAS node run; 0 = no BLAS
    uint     blas_node_base;  // element base of the node run in velk_blas_nodes
    uint     cluster_word_base; // element base of this primitive's clusters in velk_cluster_words
    uint     surface_word_base; // its tree's surface table in velk_cluster_words: blas_surface_table
    uint     _pad_static0;
    uint     _pad_static1;
    uint     _pad_static2;
};

)"
R"(
// Words per plain item in a view's culling (instance index, slot, table, draw, shape kind,
// mesh ray data): kPlainItemWords.
#define VELK_PLAIN_ITEM_WORDS 7u
// Words before an item descriptor's meshes: kItemDescriptorHeaderWords.
#define VELK_ITEM_DESCRIPTOR_HEADER_WORDS 8u
// Words per cluster draw entry: kClusterEntryWords.
#define VELK_CLUSTER_ENTRY_WORDS VELK_CLUSTER_ENTRY_END
// Words per partition in a partition table: kCullEntryWords.
#define VELK_CULL_ENTRY_WORDS VELK_PARTITION_ENTRY_END
// A kept cut's record of a quad has this for its entry: kCutQuad.
#define VELK_CUT_QUAD 0xFFFFFFFEu
// A kept cut's record of a patch of small members for rays only: kCutRayQuad.
#define VELK_CUT_RAY_QUAD 0xFFFFFFFDu
// A kept cut's record whose batch words did not carry over when its batches were made again:
// kCutStale. Its partition chooses again before anything reads it.
#define VELK_CUT_STALE 0xFFFFFFFCu
// A kept cut's record of a cluster that holds light and is not drawn has this on its entry:
// kCutLightOnly. Test with velk_cut_light_only, since a quad's, a ray quad's and a whole mesh's
// entries have it too.
#define VELK_CUT_LIGHT_ONLY 0x80000000u
#define velk_cut_light_only(entry) ((entry) < VELK_CUT_STALE && ((entry) & VELK_CUT_LIGHT_ONLY) != 0u)
// A partition's tree leaves are the finest it has: kQuadLeavesOnly.
#define VELK_QUAD_LEAVES_ONLY 0x80000000u
// Words per partition state of a kept cut: kCutPartitionWords.
#define VELK_CUT_PARTITION_WORDS VELK_PART_END
// Words from one node of the tree over a hierarchy's clusters to the next. A node is
// kLodNodeWords, and rays' node with its children's boxes sits before each.
#define VELK_LOD_NODE_STRIDE 32u
// On a tree node's child: it is a cluster (kLodLeaf), or not there (kLodNoChild).
#define VELK_LOD_LEAF 0x80000000u
#define VELK_LOD_NO_CHILD 0xFFFFFFFFu
// In a cluster draw entry's flags: the mesh deforms (kLodDeforming).
#define VELK_LOD_DEFORMING 0x80000000u
// In a cluster draw entry's flags: its material moves vertices (kLodMoved).
#define VELK_LOD_MOVED 0x40000000u

)"
// mesh_static_base value for a mesh whose static record is not resolvable yet
// (geometry not uploaded, no BLAS built). Mirrors kInvalidMeshStaticBase in
// gpu_data.h.
R"(
#define VELK_INVALID_MESH_STATIC 0xFFFFFFFFu
#define VELK_MESH_DOUBLE_SIDED 2u
#define VELK_MESH_DEFORMING 4u
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
    uint spare0;
    uint spare1;
    uint present_counter;
    float lod_scale;         // pixels an error of one unit covers at one unit away, over the most a drawn surface may be off by
    mat4 prev_view_projection;
    vec3 cam_moved;          // how far the camera moved since the previous frame
    uint ray_top;            // the view's top tree for rays in the culling arena, or 0xFFFFFFFF
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
// single-vertex fallback (vec2(0,0)) and the draw's flags lack VELK_DRAW_UV1, so
// this reads vertex 0.
//
// A draw by cluster reads the vertices and second uvs of the cluster this instance of the
// command draws, which its indices count from: see velk_cluster_vertex_word.
R"(
#define velk_vertex3d(root) \
    velk_unpack_vertex3d(velk_cluster_vertex_word(velk_draw(root), velk_cluster(root)) \
                         + uint(gl_VertexIndex) * VELK_VERTEX3D_WORDS)

#define velk_uv1(root) \
    velk_read_uv1(velk_cluster_uv1_word(velk_draw(root), velk_cluster(root), uint(gl_VertexIndex)))

#define velk_vertex_color(root) \
    velk_read_color(velk_cluster_color_word(velk_draw(root), velk_cluster(root), uint(gl_VertexIndex)))

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
// Per-draw header: 64 bytes of indices and counts, one record per batch in the
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
    uint flags;
    uint material_base;
    uint record_base;
    uint triangle_bits;
    uint material_id;
    uint material_words;
    uint index_base;
    uint cluster_base;
    uint color_base;
    uint record_count;
};

// VelkDrawData::flags (kDrawFlag in gpu_data.h).
#define VELK_DRAW_UV1 1u
#define VELK_DRAW_COLOR 2u
#define VELK_DRAW_QUADS 4u

layout(set = 1, binding = 15, std430) readonly buffer VelkDrawDataBuf { VelkDrawData data[]; } velk_draw_data;
layout(set = 1, binding = 16, std430) readonly buffer VelkCulling { uint data[]; } velk_culling;

// A cluster is the word of its record in its hierarchy's table, in the culling arena, or
// VELK_NO_CLUSTER for what is not drawn by cluster. The record says where the cluster's
// indices, vertices and second uvs are in the mesh-word arena; its indices count from its
// first vertex.
#define VELK_NO_CLUSTER 0xFFFFFFFFu

// Vertex stage only: the cluster this instance of the command draws.
#define velk_cluster(root)                                                           \
    (velk_draw(root).cluster_base == VELK_NO_CLUSTER                                 \
         ? VELK_NO_CLUSTER                                                           \
         : velk_culling.data[velk_draw(root).cluster_base + uint(gl_InstanceIndex)])

// The word of the first vertex that indices of `cluster` in draw `d` count from.
uint velk_cluster_vertex_word(VelkDrawData d, uint cluster)
{
    return cluster == VELK_NO_CLUSTER ? d.vbo_base : velk_culling.data[cluster + VELK_LOD_CLUSTER_VERTEX];
}

// The word of the second uv of vertex `index`. The draw's own when the cluster has none,
// which is one for all vertices when the mesh has none either.
uint velk_cluster_uv1_word(VelkDrawData d, uint cluster, uint index)
{
    uint own = cluster == VELK_NO_CLUSTER ? VELK_NO_CLUSTER : velk_culling.data[cluster + VELK_LOD_CLUSTER_UV1];
    uint each = (d.flags & VELK_DRAW_UV1) != 0u ? index * 2u : 0u;
    return own == VELK_NO_CLUSTER ? d.uv1_base + each : own + index * 2u;
}

vec2 velk_read_uv1(uint word)
{
    return vec2(velk_vertex_word(word, 0u), velk_vertex_word(word, 1u));
}

// The word of the colour of vertex `index` (IMeshPrimitive::get_color_buffer). The draw's own
// when the cluster has none, which is one white word for all vertices when the mesh has none.
uint velk_cluster_color_word(VelkDrawData d, uint cluster, uint index)
{
    uint own = cluster == VELK_NO_CLUSTER ? VELK_NO_CLUSTER : velk_culling.data[cluster + VELK_LOD_CLUSTER_COLOR];
    uint each = (d.flags & VELK_DRAW_COLOR) != 0u ? index : 0u;
    return own == VELK_NO_CLUSTER ? d.color_base + each : own + index;
}

vec4 velk_read_color(uint word)
{
    return unpackUnorm4x8(velk_mesh_words.data[word]);
}

// What each pixel shows, two words a pixel, row by row from `ids_word` in the culling arena,
// as the deferred path's visibility resolve leaves it: 0 for nothing, else its draw data
// record plus one. Drawn by the hardware, the second word is the position in the draw's
// visible list shifted up by its triangle bits, over the triangle. Drawn in compute
// (instance_tiny_draw_src), the first word also has VELK_IDS_TINY and the triangle in the
// cluster at VELK_IDS_TRIANGLE_SHIFT, and the second is the word of the cluster's record.
#define VELK_IDS_TINY 0x80000000u
#define VELK_IDS_TRIANGLE_SHIFT 23u
#define VELK_IDS_DRAW_MASK 0x7FFFFFu

uvec2 velk_pixel_ids(uint ids_word, uint width, ivec2 coord)
{
    uint at = ids_word + 2u * (uint(coord.y) * width + uint(coord.x));
    return uvec2(velk_culling.data[at], velk_culling.data[at + 1u]);
}

// The draw data record of a pixel that shows something.
uint velk_ids_draw(uvec2 ids)
{
    return (ids.x & VELK_IDS_DRAW_MASK) - 1u;
}

// Which part of its draw a pixel shows: its position in the visible list, or its cluster's
// record. Equal for pixels of the same instance's same cluster.
uint velk_ids_part(VelkDrawData d, uvec2 ids)
{
    return (ids.x & VELK_IDS_TINY) != 0u ? ids.y : ids.y >> d.triangle_bits;
}

// The slot of the instance a pixel shows, under its draw's instances_base.
uint velk_ids_slot(VelkDrawData d, uvec2 ids)
{
    if ((ids.x & VELK_IDS_TINY) != 0u) return velk_culling.data[ids.y + 2u];
    uint position = ids.y >> d.triangle_bits;
    return d.visible_base == VELK_NO_VISIBLE_LIST ? position : velk_culling.data[d.visible_base + position];
}

// The triangle a pixel shows, counted from its cluster's first index, or from the draw's
// first vertex or index when it is not drawn by cluster.
uint velk_ids_triangle(VelkDrawData d, uvec2 ids)
{
    if ((ids.x & VELK_IDS_TINY) != 0u) return (ids.x & ~VELK_IDS_TINY) >> VELK_IDS_TRIANGLE_SHIFT;
    return ids.y & ((1u << d.triangle_bits) - 1u);
}

// The cluster a pixel shows, or VELK_NO_CLUSTER when it is not drawn by cluster.
uint velk_ids_cluster(VelkDrawData d, uvec2 ids)
{
    if ((ids.x & VELK_IDS_TINY) != 0u) return velk_culling.data[ids.y + 1u];
    if (d.cluster_base == VELK_NO_CLUSTER) return VELK_NO_CLUSTER;
    return velk_culling.data[d.cluster_base + (ids.y >> d.triangle_bits)];
}

// The word of the first index of `cluster`, under the draw's index_base. 0 for none.
uint velk_cluster_first_index(uint cluster)
{
    return cluster == VELK_NO_CLUSTER ? 0u : velk_culling.data[cluster];
}

// A kept cut record (VELK_CUT_RECORD), as the word of it in the culling arena, or none.
#define VELK_NO_RECORD 0xFFFFFFFFu

// The kept cut record what a pixel shows is from, or VELK_NO_RECORD for what was not drawn from
// one. Valid for the frame the pixel was drawn in: records move between frames.
uint velk_ids_record(VelkDrawData d, uvec2 ids)
{
    uint position = ids.y >> d.triangle_bits;
    uint record = (ids.x & VELK_IDS_TINY) != 0u ? velk_culling.data[ids.y + VELK_TINY_CUT_RECORD]
                : d.record_base == VELK_NO_VISIBLE_LIST || position >= d.record_count ? VELK_NO_RECORD
                : velk_culling.data[d.record_base + position];
    // No record is ever at word 0, which a list that was cleared holds.
    return record == 0u ? VELK_NO_RECORD : record;
}

// How far the surface of `cluster` (VELK_NO_CLUSTER for a whole mesh) may be from the full
// mesh, in world units, for an instance whose largest axis is scaled by `scale`.
float velk_cluster_error(uint cluster, float scale)
{
    return cluster == VELK_NO_CLUSTER ? 0.0 : uintBitsToFloat(velk_culling.data[cluster + 7u]) * scale;
}
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
// vertex stage always writes all nine.
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
    layout(location = 8) flat out uint v_instance;

#define VELK_VARYINGS_IN                             \
    layout(location = 0) in vec4 v_color;            \
    layout(location = 1) in vec2 v_local_uv;         \
    layout(location = 2) flat in vec2 v_size;        \
    layout(location = 3) in vec3 v_rel_pos;          \
    layout(location = 4) in vec3 v_world_normal;     \
    layout(location = 5) flat in uint v_shape_param; \
    layout(location = 6) in vec2 v_uv1;              \
    layout(location = 7) in vec4 v_world_tangent;    \
    layout(location = 8) flat in uint v_instance;

)"
// VELK_FRAG_OUT is the single-target colour output, for forward and any full
// fragment shader. It takes the name so the identifier a shader assigns to is
// one it declared:
//   VELK_FRAG_OUT(frag_color)
//
// VELK_VISIBILITY_OUT is the visibility buffer's attachments, and
// VELK_WRITE_VISIBILITY(root) fills them: the draw record's index plus one, 0 where
// nothing was drawn, then the draw instance and the triangle, split at the record's
// triangle_bits, and the depth.
R"(
#define VELK_FRAG_OUT(Name) layout(location = 0) out vec4 Name;

#define VELK_VISIBILITY_OUT                    \
    layout(location = 0) out uvec2 g_visibility; \
    layout(location = 1) out float g_depth;

#define VELK_WRITE_VISIBILITY(root)                                                                              \
    g_visibility = uvec2((root).draw_base + 1u, (v_instance << velk_draw(root).triangle_bits) | uint(gl_PrimitiveID)); \
    g_depth = gl_FragCoord.z
)";

} // namespace velk
