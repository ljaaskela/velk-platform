#ifndef VELK_RENDER_GPU_DATA_H
#define VELK_RENDER_GPU_DATA_H

#include <velk/array_view.h>
#include <velk/string_view.h>

#include <velk-render/frame/cull_layout.h>

#include <cmath>
#include <cstdint>

namespace velk {

/// @file gpu_data.h
/// Framework-level GPU data structures.
///
/// The universal per-instance type (`ElementInstance`) lives in
/// `velk-scene/instance_types.h`; visuals fill its fields and the
/// renderer's batch builder writes the world matrix.

/// Declares a struct with std430-compatible alignment (16 bytes).
/// Use for material GPU data structs that follow the DrawDataHeader.
/// The compiler pads the struct automatically, no manual _pad fields needed.
#define VELK_GPU_STRUCT struct alignas(16)

/// Renderer-side description of how a view reaches the scene BVH for
/// index-based shader reads. Carried through RenderView / FrameContext and
/// stamped (flat) into FrameGlobals / RtRoot. All indices are absolute in the
/// shared node and record arenas. Not uploaded directly; the GPU structs mirror
/// these fields individually.
/// A box in world space.
struct WorldBox
{
    float min[3];
    float max[3];
};

struct BvhBinding
{
    /// Where something rays can hit changed this frame: the bounds of each thing that moved,
    /// before and after, appeared or went away. Valid for the frame. See IBvh::changed_bounds.
    array_view<const WorldBox> changed;
    /// Everything may have changed, and `changed` does not list it.
    bool changed_all = false;
    /// Where each object is, by the slot of its id, and how many slots. See
    /// IBvh::get_object_transforms.
    uint32_t object_transforms = 0xFFFFFFFFu;
    uint32_t object_slots = 0;
    /// Its plain objects' partitions and how many entries their table has. See
    /// IBvh::get_plain_partitions.
    uint32_t plain_partitions = 0xFFFFFFFFu;
    uint32_t plain_partition_count = 0;
};

/// Per-frame global data written by the renderer, read by all shaders.
///
/// Layout must match the `GlobalData` struct declaration in velk.glsl
/// (scalar layout). The view preparer writes one of these per view per
/// frame into its persistent region of the shared globals arena
/// (set = 1 slot 2); shaders read it as `velk_globals.data[base]`.
struct FrameGlobals
{
    float    view_projection[16];          ///< View-projection for positions relative to the camera.
    float    inverse_view_projection[16];  ///< Inverse of view_projection.
    float    viewport[4];                  ///< width, height, 1/width, 1/height.
    float    cam_pos[4];                   ///< World-space camera position (xyz) + pad.
    uint32_t spare0;
    uint32_t spare1;
    uint32_t present_counter;              ///< Monotonic CPU frame index (RT noise seed; never a GPU-completion proxy).
    float    lod_scale;                    ///< See set_lod_scale.
    float    prev_view_projection[16];     ///< Previous frame's view-projection (identity on the first frame). For temporal reprojection.
    float    cam_moved[3];                 ///< How far the camera moved since the previous frame.
    /// The word of the view's top tree for rays in the culling arena (kRayTopHeaderWords),
    /// which its render path writes after culling; 0xFFFFFFFF for none.
    uint32_t ray_top;
};

static_assert(sizeof(FrameGlobals) == 256, "FrameGlobals layout must match velk.glsl");

/// How far a level of detail may be from the full mesh on screen, in pixels.
inline constexpr float kLodMaxErrorPixels = 1.f;
/// The longest a drawn triangle's edge, or a quad's diagonal, may be on screen, in pixels: a
/// level of detail is taken while its triangles are within it, and a group of instances
/// becomes one quad once that quad is. Built into the hierarchy as a cluster's error: its
/// level's longest edge over this, times kLodMaxErrorPixels.
inline constexpr float kLodElementPixels = 4.f;
/// How far apart points of light are on screen, in pixels: one about every 4 by 4 pixels, the
/// size elements are drawn at, so that light lives at the drawing level. A cluster smaller than
/// this, and the quads under a quad tree node this wide, have one value of light. See
/// light_pool.h.
inline constexpr float kLightPixels = kLodElementPixels;

/// Fills `lod_scale` from the view-projection and viewport: how many pixels an error of one
/// unit covers at one unit from the camera, over kLodMaxErrorPixels. Rays choose a mesh's
/// level of detail by it, as drawing does.
inline void set_lod_scale(FrameGlobals& globals)
{
    const float* m = globals.view_projection;
    const float pixels_per_unit = std::sqrt(m[1] * m[1] + m[5] * m[5] + m[9] * m[9]) * globals.viewport[1] * 0.5f;
    globals.lod_scale = pixels_per_unit / kLodMaxErrorPixels;
}

/// DrawDataHeader::visible_base of a draw with no visible list. Mirrors GLSL VELK_NO_VISIBLE_LIST.
inline constexpr uint32_t kNoVisibleList = 0xFFFFFFFFu;

/**
 * @brief Standard draw data header at the start of every draw's GPU data.
 *
 * 64 bytes via VELK_GPU_STRUCT (16-byte aligned for std430). Every field is
 * an index or a count.
 *
 * Material data does not trail the header: it lives in the set = 1 material
 * arena, indexed by @c material_base (region offset / material record size).
 *
 * Multi-texture materials (e.g. StandardMaterial) embed their bindless
 * TextureIds directly in their own UBO via ITextureResolver, so the
 * header carries only the single texture_id used by simple visuals
 * (image, texture, text).
 */
VELK_GPU_STRUCT DrawDataHeader
{
    uint32_t globals_base;      ///< Index into the set = 1 globals buffer; shaders read velk_globals.data[globals_base].
    uint32_t instances_base;    ///< Element base into the set = 1 instance arena.
    uint32_t texture_id;        ///< Bindless texture index (0 = none).
    uint32_t visible_base;      ///< Word base of the list of visible instances in the set = 1 culling arena, or kNoVisibleList when the draw's instances are all drawn in order.
    uint32_t vbo_base;          ///< Word base of the draw's vertex stream in the set = 1 mesh-word arena.
    uint32_t uv1_base;          ///< Word base of the draw's TEXCOORD_1 stream, or of a context-owned single-vertex fallback without kDrawFlagUv1.
    uint32_t flags;             ///< kDrawFlag bits.
    uint32_t material_base;     ///< Element base into the set = 1 material arena; fragment shader reads velk_materials.data[material_base].
    uint32_t record_base;       ///< Word base in the culling arena of the kept cut record each draw instance is from (kCutRecordWords), as the word of the record, kNoVisibleList for an instance from none; or kNoVisibleList when the draw has no such list. Read by velk_ids_record.
    uint32_t triangle_bits;     ///< Low bits of a visibility ID's second word that hold the triangle. The rest hold the draw instance.
    uint32_t material_id;       ///< Which material eval the visibility resolve calls, 0 for the default.
    uint32_t material_words;    ///< Word base of the material's record, as compute reads it.
    uint32_t index_base;        ///< Word base of the draw's indices in the mesh-word arena, or kNoVisibleList when it has none.
    uint32_t cluster_base;      ///< Word base in the culling arena of each list position's cluster, as the word there of its record in its hierarchy's table, or kNoVisibleList when not drawn by cluster.
    uint32_t color_base;        ///< Word base of the draw's colour stream (IMeshPrimitive::get_color_buffer), or of a context-owned white word without kDrawFlagColor.
    uint32_t record_count;      ///< How many positions the column at record_base has; a draw instance past it is from no record.
};

static_assert(sizeof(DrawDataHeader) == 64, "DrawDataHeader must be 64 bytes for std430 alignment");

/// DrawDataHeader::flags: the draw has a second uv per vertex at `uv1_base`; without, every
/// vertex reads the one there. Mirrors VELK_DRAW_UV1.
inline constexpr uint32_t kDrawFlagUv1 = 1u << 0;
/// DrawDataHeader::flags: the draw has a colour per vertex at `color_base`; without, every vertex
/// reads the white word there. Mirrors VELK_DRAW_COLOR.
inline constexpr uint32_t kDrawFlagColor = 1u << 1;
/// DrawDataHeader::flags: the draw is of a view's quads: its instances are quads (kQuadNodeWords),
/// its visible list the words of the quads drawn, and each an instance of the unit quad. Mirrors
/// VELK_DRAW_QUADS.
inline constexpr uint32_t kDrawFlagQuads = 1u << 2;

/// Words of a draw's shading summary in the draw-shade array (IRenderBackend::kGlobalDrawShade),
/// at its draw record's index: what a ray that hits it sees without its material's code. The
/// first word its material's average albedo (IMaterial::get_average_albedo) as sRGB bytes, red
/// lowest, with kDrawShadeUnlit in the top byte; the second its average emission as RGB9E5. A
/// hit multiplies the albedo by its instance's colour and, with kDrawFlagColor, its vertex
/// colours: reflected when lit, given off when unlit. Mirrors VELK_DRAW_SHADE_*.
inline constexpr uint32_t kDrawShadeWords = 2;
/// The draw's material is unlit (IMaterial::is_lit false).
inline constexpr uint32_t kDrawShadeUnlit = 1u << 24;

/// A linear value 0 to 1 as an sRGB byte.
inline uint32_t srgb_byte(float linear)
{
    const float c = linear <= 0.f ? 0.f : linear >= 1.f ? 1.f : linear;
    const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
    return static_cast<uint32_t>(s * 255.f + 0.5f);
}

/// A linear colour as RGB9E5: three 9-bit mantissas, red lowest, under a shared 5-bit
/// exponent (bias 15), as VK_FORMAT_E5B9G9R8_UFLOAT_PACK32 holds it.
inline uint32_t rgb9e5(float r, float g, float b)
{
    const auto clamp = [](float v) {
        constexpr float kMax = 65408.f;  // (2^9 - 1) / 2^9 * 2^16
        return v > 0.f ? (v < kMax ? v : kMax) : 0.f;
    };
    r = clamp(r);
    g = clamp(g);
    b = clamp(b);
    const float most = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int exp = most > 0.f ? static_cast<int>(std::floor(std::log2(most))) + 1 : 0;
    if (exp < -15) exp = -15;
    if (exp > 16) exp = 16;
    float scale = std::exp2(static_cast<float>(9 - exp));
    // Rounding can carry the largest mantissa to 512: one exponent more.
    if (static_cast<uint32_t>(most * scale + 0.5f) >= 512u && exp < 16) {
        ++exp;
        scale *= 0.5f;
    }
    const auto mantissa = [scale](float v) {
        const uint32_t m = static_cast<uint32_t>(v * scale + 0.5f);
        return m > 511u ? 511u : m;
    };
    return mantissa(r) | (mantissa(g) << 9) | (mantissa(b) << 18) | (static_cast<uint32_t>(exp + 15) << 27);
}

/// Packs a draw's shading summary (kDrawShadeWords) into @p out.
inline void pack_draw_shade(float albedo_r, float albedo_g, float albedo_b, bool lit, float emission_r,
                            float emission_g, float emission_b, uint32_t out[kDrawShadeWords])
{
    out[0] = srgb_byte(albedo_r) | (srgb_byte(albedo_g) << 8) | (srgb_byte(albedo_b) << 16) | (lit ? 0u : kDrawShadeUnlit);
    out[1] = rgb9e5(emission_r, emission_g, emission_b);
}

/// Words before the quads in a view's quad list: VELK_QUAD_LIST (frame/cull_layout.h).
inline constexpr uint32_t kQuadListHeaderWords = kQuadListQuads;

// ===== Scene-data GPU structs =====
// Mirrors of GLSL types consumed by RT and deferred compute shaders.
// Plain POD, no scene deps; packed for std430 indexed reads.

/// A shape in its element's local space, as the scene emits it to make its records and
/// partitions from. Mirrors the RtShape struct in velk.glsl, which rays build from an
/// instance to test an analytic visual. 128 bytes.
///
/// shape_kind:
///   0 = rect, 1 = cube, 2 = sphere: analytic primitives.
///   255 = mesh: `origin/u_axis` carry the local AABB.
///   3..254 reserved for future analytic kinds.
VELK_GPU_STRUCT RtShape
{
    float    origin[4];       ///< xyz = local origin (corner for rect/cube, AABB corner for sphere, AABB min for mesh)
    float    u_axis[4];       ///< xyz = local x axis scaled by width (AABB max for mesh)
    float    v_axis[4];       ///< xyz = local y axis scaled by height
    float    w_axis[4];       ///< xyz = local z axis scaled by depth (cube only; zero otherwise)
    float    color[4];        ///< rgba base color (used when material_id == 0)
    float    params[4];       ///< x = corner radius (rect) or sphere radius; yzw reserved
    uint32_t material_id;     ///< 0 = no material (use color); otherwise dispatched via switch
    uint32_t texture_id;      ///< bindless index, 0 when unused
    uint32_t shape_param;     ///< per-shape material data (e.g. glyph index for text)
    uint32_t shape_kind;      ///< 0 = rect, 1 = cube, 2 = sphere, 255 = mesh
    uint32_t material_base;   ///< Word index of its material's record in the set = 1 material arena.
    uint32_t _pad0;
    uint32_t _pad1;
    uint32_t _pad2;
};
static_assert(sizeof(RtShape) == 128, "RtShape layout mismatch");

/// Sentinel value of RtShape::shape_kind for triangle-mesh shapes.
inline constexpr uint32_t kRtShapeKindMesh = 255;

/// Mesh-static metadata: same for every element instance referencing a
/// given IMeshPrimitive. The primitive owns a persistent region of the
/// shared mesh-static arena, so the element index is stable across frames
/// and instances can cache it. The BLAS it describes lives in its own
/// arenas, reached by `blas_node_base` / `cluster_word_base` rather than by
/// trailing this record. Mirrors GLSL `MeshStaticData`.
VELK_GPU_STRUCT MeshStaticData
{
    uint32_t vbo_base;        ///< word base of this primitive's first vertex in the mesh-word arena.
    uint32_t flags;           ///< kMeshStaticDoubleSided: it is not a closed solid. kMeshStaticDeforming: its tree's stored normals may be out of date.
    uint32_t triangle_count;
    uint32_t vertex_stride;   ///< bytes per vertex, from the primitive (48 for VelkVertex3D).
    uint32_t blas_root;       ///< root index within this primitive's BLAS node run.
    uint32_t blas_node_count; ///< length of this primitive's BLAS node run; 0 = no BLAS.
    uint32_t blas_node_base;  ///< element base of the node run in the BLAS node arena.
    uint32_t cluster_word_base; ///< element base of this primitive's clusters in the cluster word arena.
    uint32_t surface_word_base; ///< its tree's surface table (blas_surface_table) in the cluster word arena.
    uint32_t pad[3];
};
static_assert(sizeof(MeshStaticData) == 48, "MeshStaticData layout mismatch");

/// `MeshStaticData::flags` bit for a primitive seen from both sides. Mirrors GLSL `VELK_MESH_DOUBLE_SIDED`.
inline constexpr uint32_t kMeshStaticDoubleSided = 2u;
/// `MeshStaticData::flags` bit for a primitive whose vertices change. Mirrors GLSL `VELK_MESH_DEFORMING`.
inline constexpr uint32_t kMeshStaticDeforming = 4u;

/// A mesh's ray data (MeshStaticData's index) that is not there yet: its geometry is not
/// uploaded or its tree not built. Rays skip it, and the CPU asks again on a later frame.
inline constexpr uint32_t kInvalidMeshStaticBase = 0xFFFFFFFFu;

/// GPU-side scene light. Mirrors the `Light` struct in the compute
/// shaders (80 bytes). `flags.y` is shadow_tech_id; callers populate
/// it after resolving their shadow technique registry.
VELK_GPU_STRUCT GpuLight
{
    uint32_t flags[4];            ///< x = LightType, y = shadow_tech_id, zw = _
    float    position[4];         ///< xyz = world position (point / spot)
    float    direction[4];        ///< xyz = world forward (dir / spot)
    float    color_intensity[4];  ///< rgb = colour, a = intensity multiplier
    float    params[4];           ///< x = range, y = cos(inner), z = cos(outer), w = light size (dir: angular radius rad; point/spot: world radius)
};
static_assert(sizeof(GpuLight) == 80, "GpuLight layout mismatch");

} // namespace velk

#endif // VELK_RENDER_GPU_DATA_H
