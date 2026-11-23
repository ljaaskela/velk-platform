#ifndef VELK_SAMPLES_NATURE_TREES_H
#define VELK_SAMPLES_NATURE_TREES_H

// Generated trees as velk_ui_scale's nature has them, for the samples that show them.

#include <velk/api/velk.h>
#include <velk/vector.h>
#include <velk-render/api/render_context.h>
#include <velk-render/ext/element_vertex.h>
#include <velk-render/ext/material.h>
#include <velk-render/gpu_data.h>

#include <cstdint>

namespace velk_samples {

/// The samples' vertex: position, normal, first uv and tangent.
struct Vertex
{
    float position[3];
    float normal[3];
    float uv[2];
    float tangent[4];
};

class ITreeMaterial : public velk::Interface<ITreeMaterial>
{
public:
    VELK_INTERFACE(
        (PROP, velk::color, albedo, velk::color{}),
        (PROP, float, roughness, 0.8f),
        (PROP, bool, wind, true)
    )
};

inline constexpr velk::Uid kTreeMaterialClass{"baac361e-72bd-42a6-9639-5f5b8ba5a643"};

VELK_GPU_STRUCT TreeParams
{
    velk::color albedo;
    float roughness;
};

inline constexpr velk::string_view kTreeEvalSrc = R"(
struct TreeData {
    vec4 albedo;
    vec4 roughness_pad;
};
VELK_MATERIAL(TreeData)

MaterialEval velk_eval_tree(EvalContext ctx)
{
    TreeData d = VELK_LOAD_MATERIAL(TreeData, ctx);
    MaterialEval e = velk_default_material_eval();
    e.color = d.albedo;
    e.normal = ctx.normal;
    e.roughness = d.roughness_pad.x;
    e.lighting_mode = VELK_LIGHTING_STANDARD;
    return e;
}
)";

// Branches bend further the further out they are, and leaves, whose first uv is 1, move on
// their own. Rays see the mesh at rest.
inline constexpr velk::string_view kTreeVertexFn = R"(
VelkVaryings velk_vertex_tree(VelkVertex3D v, ElementInstance inst, GlobalData globals)
{
    float t = float(globals.present_counter) / 60.0;
    vec3 at = inst.world_matrix[3].xyz;
    float phase = at.x * 0.013 + at.z * 0.017;
    vec3 p = v.position;
    float out_from_trunk = length(p.xz);
    p.y += sin(t * 1.9 + phase + out_from_trunk * 0.15) * out_from_trunk * 0.03;
    p.x += sin(t * 1.3 + phase + p.y * 0.05) * out_from_trunk * 0.02;
    p += vec3(sin(t * 5.0 + p.x * 0.7 + p.y), sin(t * 6.0 + p.z * 0.9), sin(t * 5.5 + p.y * 0.8)) * 0.35 * v.uv.x;

    v.position = p;
    return velk_default_varyings(v, inst, globals);
}
)";

/// A generated tree's wood or leaves. Register it (register_type<TreeMaterial>) once before
/// making any.
class TreeMaterial : public velk::ext::Material<TreeMaterial, ITreeMaterial>
{
public:
    VELK_CLASS_UID(kTreeMaterialClass, "ScaleTreeMaterial");

    size_t get_draw_data_size() const override { return sizeof(TreeParams); }
    bool is_lit() const override { return true; }

    velk::ReturnValue write_draw_data(void* out, size_t size, velk::ITextureResolver*) const override
    {
        auto state = velk::read_state<ITreeMaterial>(this);
        if (!state) {
            return velk::ReturnValue::Fail;
        }
        return set_material<TreeParams>(out, size, [&](auto& p) {
            p.albedo = state->albedo;
            p.roughness = state->roughness;
        });
    }

    velk::color get_average_albedo() const override
    {
        auto state = velk::read_state<ITreeMaterial>(this);
        return state ? state->albedo : velk::color::white();
    }

    velk::string_view get_source(velk::string_view role) const override
    {
        if (role == velk::shader_role::kEval) return kTreeEvalSrc;
        auto state = velk::read_state<ITreeMaterial>(this);
        if (state && state->wind) {
            if (role == velk::shader_role::kVertexFunction) return kTreeVertexFn;
            if (role == velk::shader_role::kVertex) {
                if (vertex_src_.empty()) {
                    vertex_src_ = velk::ext::velk_vertex_shader_with(kTreeVertexFn, "velk_vertex_tree");
                }
                return vertex_src_;
            }
        }
        return velk::ext::Material<TreeMaterial, ITreeMaterial>::get_source(role);
    }

    velk::string_view get_fn_name(velk::string_view role) const override
    {
        if (role == velk::shader_role::kVertexFunction) return "velk_vertex_tree";
        return role == velk::shader_role::kEval ? velk::string_view("velk_eval_tree") : velk::string_view{};
    }

private:
    mutable velk::string vertex_src_;
};

/// A tree material of colour @p c, its wind on or off, drawn from both sides or not.
velk::IMaterial::Ptr tree_material(const velk::color& c, float roughness, bool double_sided, bool wind);

/// How a generated tree grows. Lengths are in world units.
struct TreeShape
{
    uint32_t seed;
    float trunk_length;
    float trunk_radius;
    int levels;             ///< Levels of branches on the trunk.
    int children;           ///< Branches on each branch.
    float first_child;      ///< How far along its parent the first branch starts, from 0 to 1.
    float spread;           ///< Angle of a branch off its parent, in radians.
    float length_ratio;     ///< A branch's length over its parent's.
    float radius_ratio;     ///< A branch's radius over its parent's at that point.
    float rise;             ///< How much branches turn upwards as they grow. Below 0 they droop.
    bool taper_with_height; ///< Branches shorten towards the top of the parent, as on a conifer.
    int leaves;             ///< Leaves on each branch of the last level.
    float leaf_length;
    float leaf_width;
};

/// Grows a tree on y = 0 towards -y, as tubes for its wood and separate leaves.
class TreeGrower
{
public:
    explicit TreeGrower(const TreeShape& shape) : shape_(shape), state_(shape.seed) {}

    velk::IMesh::Ptr build(velk::IRenderContext& ctx, const velk::IMaterial::Ptr& bark, const velk::IMaterial::Ptr& foliage);

    /// A primitive of @p vertices and @p indices (a triangle list) with @p material, and its tree for rays.
    static velk::IMeshPrimitive::Ptr make_primitive(velk::IRenderContext& ctx, const velk::vector<Vertex>& vertices,
                                                    const velk::vector<uint32_t>& indices, const velk::IMaterial::Ptr& material);

private:
    float random();
    velk::vec3 random_direction();
    void grow(int level, const velk::vec3& base, const velk::vec3& direction, float length, float radius);
    void add_leaf(const velk::vec3& at, const velk::vec3& direction);
    void shade_leaves();

    TreeShape shape_;
    uint32_t state_;
    velk::vector<Vertex> wood_;
    velk::vector<uint32_t> wood_indices_;
    velk::vector<Vertex> leaves_;
    velk::vector<uint32_t> leaf_indices_;
};

/// Nature's three species, oak, birch and pine, as one mesh each: bark one sided, foliage two
/// sided. @p leaf_density times as many leaves, larger or smaller to keep a crown's area.
struct NatureTrees
{
    velk::IMesh::Ptr oak;
    velk::IMesh::Ptr birch;
    velk::IMesh::Ptr pine;
};
NatureTrees build_nature_trees(velk::IRenderContext& ctx, bool wind, float leaf_density = 1.f);

} // namespace velk_samples

#endif // VELK_SAMPLES_NATURE_TREES_H
