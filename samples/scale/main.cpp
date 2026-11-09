// Scale test for moving and deforming content. Builds its scene in code, so
// it loads no assets.
//
// GI test scenes: velk_ui_scale gitest <test> [frames]. See gi_test.cpp.
//
// Usage: velk_ui_scale [elements] [movers] [frames]
//                      [translate|rotate|group|escape|burst|churn|recolor|deform|scatter
//                       |forest|forest_elements|forest_shift|forest_churn|forest_cluster|nature]
//                      [unlimited] [rt] [text] [overlay] [near] [species] [wind] [gpu] [outlier] [mixed] [flyby] [heavy] [real] [lowsun] [dense] [blocks] [rocks] [across] [top] [yaw=N] [distance=N] [pitch=N] [budget=MB]
//
// elements: visuals in the grid, an equal mix of triangle meshes, rects, cubes
//           and spheres. Default 1000.
// movers:   how many of them move every frame. Default 10.
// frames:   exits after this many frames. Default 0, which runs until the
//           window closes.
// rotate:   movers rotate in place, with no change in translation. The default
//           is translate.
// group:    movers translate a parent that has no visual, and the visual sits on
//           its child.
// escape:   movers rise far above the scene and come back, which grows the GI
//           probe volume and then shrinks it. The first 190 frames show both.
// burst:    movers translate for 200 frames, then stay still for 200, and repeat.
// churn:    movers translate, and every 20 frames one extra cube is removed and
//           another added.
// recolor:  movers translate, and at frame 10 the rects turn from grey to red with
//           no change in geometry.
// deform:   movers are spheres of about 65,000 triangles with a ripple running over
//           them. The perf log has the cost under renderer.deform.
// scatter:  like deform, and the vertices also fly apart, until the trees degrade
//           enough to be built again, shown under mesh.blas_rebuild.
// forest:   elements is the number of trees, all on one instanced visual, on a
//           jittered grid instead of the mixed visuals. Movers are trees scattered
//           over the forest, moved every frame, and the camera is close to the center.
//           Trees have two primitives and 24 triangles.
// forest_elements: the same trees as one element each, to compare against forest.
// forest_shift: forest, with the whole element translated every frame.
// forest_churn: forest, with one tree removed and another added every frame.
// forest_cluster: forest, with the movers packed in a cube in the air above the center.
// nature:   elements is the number of trees, spread over generated mountains of a
//           fixed size. Right drag turns the camera, W, A, S and D fly it, and the wheel
//           jumps along the view.
// outlier:  forest_cluster, with one mover 2,000,000 units away, which stretches the
//           partition's bounds until the cube is a tiny part of them.
// mixed:    a forest's movers vary in size: one in 100 at 30 times, one in 10 at 5 times,
//           and the rest at half size.
// flyby:    the camera flies over a forest and repeats: over the crowns, then across at
//           altitude, then straight down from where the whole forest shows, moving between
//           them. One round is 1620 frames, and GPU timings and frame times print per section.
// unlimited: turns vsync off, so frame times show the real cost.
// rt:       uses the ray-traced render path instead of deferred.
// text:     adds a text visual above the grid. It moves like the movers when
//           there are any.
// near:     a forest's trees are drawn up to a distance of 3000. Rays still see them all.
// species:  a forest has three meshes of tree, of different heights. Its movers do not
//           move.
// wind:     a forest's trees sway on the GPU, with gusts crossing the forest.
// gpu:      deform runs the same ripple on the GPU instead of the CPU.
// heavy:    a forest's tree has about 10,000 triangles instead of 24. All of them are drawn
//           if in view, so start with few trees, or `near`.
// real:     a forest draws and traces every tree however small, to compare against.
// lowsun:   the sun is at 15 degrees instead of 35.
// dense:    a forest's trees are 30 apart instead of 80, so the canopy nearly closes.
// blocks:   a forest's mesh is a flat red box instead of a tree.
// rocks:    a forest's mesh is a lumpy rock of about 25,000 triangles instead of a tree.
// budget=MB: limits the GPU memory a forest's instances use.
// across, top: the camera stays still at that stop of the flyby, facing `yaw=N` degrees,
//           with `distance=N` and `pitch=N` replacing the stop's own.
// grass=N:  nature has N patches of grass, 1,000,000 by default, fewer on steep or high ground.
// size=WxH: the window's size.
// notour:   nature's camera starts still, rather than following a loop until there is input.
// tour=N:   the loop starts N units along.
// nosky:    nature has no generated sky.
// lodtest=N: nature is 10 trees of species N (0 oak, 1 birch, 2 conifer) at growing distances
//           from a still camera, on flat ground, with no grass or wind.
// nowind:   nature's trees stand still.
// noraywind: rays follow only the whole of nature's trees, not their branches.
// noshadow: the sun casts no ray-traced shadows.
// flat:     nature's ground has no height.
// sun=N:    the sun is N degrees above the horizon.
// leaves=N: nature's trees have N times as many leaves, larger or smaller to keep their area.
// view=x,y,z,yaw,pitch,distance: starts the camera there. C logs the current one.
// overlay:  shows the performance overlay. Off by default, since its numbers change
//           from run to run and captures get compared.
//
// gicell=N: the finest GI probe cells are N wide, instead of being chosen from the view.
//
// Prints GPU pass timings on exit. The engine's perf log, also printed on exit,
// has the BVH timings under renderer.bvh_*. The first frames are excluded from
// the GPU timings because they include pipeline creation.

#include <velk/api/perf.h>
#include <velk/api/velk.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <velk-render/api/material/standard_material.h>
#include <velk-render/api/render_context.h>
#include <velk-render/api/shadow_technique.h>
#include <velk-render/blas.h>
#include <velk-render/ext/material.h>
#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_camera.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-runtime/api/application.h>
#include <velk-scene/api/camera.h>
#include <velk-scene/api/element.h>
#include <velk-scene/api/mesh.h>
#include <velk-scene/api/post_process.h>
#include <velk-scene/api/render_path.h>
#include <velk-scene/api/scene.h>
#include <velk-scene/api/trait/compute_mesh_deformer.h>
#include <velk-scene/api/trait/light.h>
#include <velk-scene/api/trait/orbit.h>
#include <velk-scene/api/trait/trs.h>
#include <velk-scene/api/visual/cube.h>
#include <velk-scene/api/visual/instanced_mesh.h>
#include <velk-scene/api/visual/mesh.h>
#include <velk-scene/api/visual/sphere.h>
#include <velk-scene/ext/trait.h>
#include <velk-ui/api/trait/fixed_size.h>
#include <velk-ui/api/visual/rect.h>
#include <velk-ui/plugins/image/api/environment.h>
#include <velk-ui/plugins/text/api/text_visual.h>
#include <velk/interface/resource/intf_resource_protocol.h>
#include <velk/interface/resource/intf_resource_store.h>

namespace {

constexpr float kElementSize = 50.f;
constexpr float kElementSpacing = 80.f;
constexpr float kFloorY = 200.f;
constexpr long kWarmupFrames = 10;

struct Vertex
{
    float position[3];
    float normal[3];
    float uv[2];
    float tangent[4];
};

class IRipple : public velk::Interface<IRipple>
{
public:
    VELK_INTERFACE(
        (PROP, float, time, 0.f),
        (PROP, float, scatter, 0.f)
    )
};

inline constexpr velk::Uid kRippleClass{"d864fbf7-9279-47f3-8dda-319b1afc8f6f"};

/// Runs a ripple along the normals, and moves each vertex away by its own offset.
class Ripple : public velk::ext::MeshDeformer<Ripple, IRipple>
{
public:
    VELK_CLASS_UID(kRippleClass, "ScaleRipple");

    void deform(velk::array_view<const velk::MeshVertex> rest, velk::MeshVertex* vertices) override
    {
        constexpr float kAmount = 4.f;
        constexpr float kWavesPerUnit = 0.25f;
        auto state = velk::read_state<IRipple>(this);
        for (size_t i = 0; i < rest.size(); ++i) {
            const velk::vec3 p = rest[i].position;
            const float wave = std::sin(p.y * kWavesPerUnit + state->time * 4.f);
            // Three unrelated values per vertex, from -1 to 1.
            const auto f = static_cast<float>(i);
            const velk::vec3 away{std::sin(f * 12.9898f), std::sin(f * 78.233f), std::sin(f * 37.719f)};
            vertices[i].position = p + rest[i].normal * (wave * kAmount) + away * state->scatter;
        }
    }
};

inline constexpr velk::Uid kTerrainMaterialClass{"59e864b6-ef1d-4c5d-a007-66abfabd4451"};

VELK_GPU_STRUCT TerrainParams
{
    velk::color meadow;
    velk::color woodland;
    velk::color rock;
    velk::color snow;  ///< Its alpha is the world y of the snow line.
};

constexpr velk::string_view kTerrainEvalSrc = R"(
struct TerrainData {
    vec4 meadow;
    vec4 woodland;
    vec4 rock;
    vec4 snow;
};
VELK_MATERIAL(TerrainData)

float terrain_hash(vec2 p)
{
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}

float terrain_noise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(terrain_hash(i), terrain_hash(i + vec2(1.0, 0.0)), u.x),
               mix(terrain_hash(i + vec2(0.0, 1.0)), terrain_hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

MaterialEval velk_eval_terrain(EvalContext ctx)
{
    TerrainData d = VELK_LOAD_MATERIAL(TerrainData, ctx);
    vec3 n = normalize(ctx.normal);
    vec2 p = ctx.hit_pos.xz;
    float patches = terrain_noise(p / 900.0) * 0.6 + terrain_noise(p / 210.0) * 0.3 + terrain_noise(p / 40.0) * 0.1;
    vec3 ground = mix(d.meadow.rgb, d.woodland.rgb, smoothstep(0.35, 0.65, patches));
    // Up is -y.
    float flat_amount = smoothstep(0.86, 0.95, -n.y);

    MaterialEval e = velk_default_material_eval();
    vec3 surface = mix(d.rock.rgb, ground, flat_amount);
    // Up is -y, so higher ground has a smaller y. Snow does not hold on the steepest faces.
    float above_line = (d.snow.a - ctx.hit_pos.y + (patches - 0.5) * 500.0) / 300.0;
    float snow = clamp(above_line, 0.0, 1.0) * smoothstep(0.55, 0.75, -n.y);
    e.color = vec4(mix(surface, d.snow.rgb, snow), 1.0);
    e.normal = n;
    e.roughness = 0.9;
    e.lighting_mode = VELK_LIGHTING_STANDARD;
    return e;
}
)";

/// Ground coloured by world position and slope.
class TerrainMaterial : public velk::ext::Material<TerrainMaterial>
{
public:
    VELK_CLASS_UID(kTerrainMaterialClass, "ScaleTerrainMaterial");

    size_t get_draw_data_size() const override { return sizeof(TerrainParams); }

    velk::ReturnValue write_draw_data(void* out, size_t size, velk::ITextureResolver*) const override
    {
        return set_material<TerrainParams>(out, size, [](auto& p) {
            p.meadow = velk::color{0.20f, 0.30f, 0.09f, 1.f};
            p.woodland = velk::color{0.13f, 0.20f, 0.08f, 1.f};
            p.rock = velk::color{0.30f, 0.27f, 0.24f, 1.f};
            p.snow = velk::color{0.85f, 0.87f, 0.90f, -1500.f};
        });
    }

    velk::string_view get_source(velk::string_view role) const override
    {
        if (role == velk::shader_role::kEval) return kTerrainEvalSrc;
        return velk::ext::Material<TerrainMaterial>::get_source(role);
    }

    velk::string_view get_fn_name(velk::string_view role) const override
    {
        return role == velk::shader_role::kEval ? velk::string_view("velk_eval_terrain") : velk::string_view{};
    }
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

constexpr velk::string_view kTreeEvalSrc = R"(
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
constexpr velk::string_view kTreeVertexFn = R"(
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

/// A generated tree's wood or leaves.
class TreeMaterial : public velk::ext::Material<TreeMaterial, ITreeMaterial>
{
public:
    VELK_CLASS_UID(kTreeMaterialClass, "ScaleTreeMaterial");

    size_t get_draw_data_size() const override { return sizeof(TreeParams); }

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

/// Generated hills, as heights on a square grid centred on the origin.
class Terrain
{
public:
    Terrain(float half_size, uint32_t cells, float height_scale)
        : half_size_(half_size), cells_(cells), cell_size_(half_size * 2.f / static_cast<float>(cells))
    {
        VELK_PERF_SCOPE("scale.terrain_heights");
        heights_.resize(static_cast<size_t>(cells + 1) * (cells + 1));
        for (uint32_t z = 0; z <= cells; ++z) {
            for (uint32_t x = 0; x <= cells; ++x) {
                heights_[index(x, z)] = generate(grid_coord(x), grid_coord(z)) * height_scale;
            }
        }
    }

    float half_size() const { return half_size_; }

    /// The ground's height above y = 0 at a point, between grid points as the mesh has it.
    float height_at(float x, float z) const
    {
        const float gx = std::clamp((x + half_size_) / cell_size_, 0.f, static_cast<float>(cells_));
        const float gz = std::clamp((z + half_size_) / cell_size_, 0.f, static_cast<float>(cells_));
        const uint32_t ix = std::min(static_cast<uint32_t>(gx), cells_ - 1);
        const uint32_t iz = std::min(static_cast<uint32_t>(gz), cells_ - 1);
        const float fx = gx - static_cast<float>(ix);
        const float fz = gz - static_cast<float>(iz);
        const float near_row = heights_[index(ix, iz)] + (heights_[index(ix + 1, iz)] - heights_[index(ix, iz)]) * fx;
        const float far_row = heights_[index(ix, iz + 1)] + (heights_[index(ix + 1, iz + 1)] - heights_[index(ix, iz + 1)]) * fx;
        return near_row + (far_row - near_row) * fz;
    }

    /// A mesh of the grid at y = base minus height, since up is -y.
    velk::IMesh::Ptr build_mesh(velk::IRenderContext& ctx, float base, const velk::IMaterial::Ptr& material) const
    {
        VELK_PERF_SCOPE("scale.terrain_mesh");
        static constexpr velk::VertexAttribute kAttributes[] = {
            {velk::VertexAttributeSemantic::Position, velk::VertexAttributeFormat::Float3, 0},
            {velk::VertexAttributeSemantic::Normal, velk::VertexAttributeFormat::Float3, 12},
            {velk::VertexAttributeSemantic::TexCoord0, velk::VertexAttributeFormat::Float2, 24},
            {velk::VertexAttributeSemantic::Tangent, velk::VertexAttributeFormat::Float4, 32},
        };
        velk::vector<Vertex> vertices;
        vertices.reserve(heights_.size());
        float lowest = heights_[0];
        float highest = heights_[0];
        for (uint32_t z = 0; z <= cells_; ++z) {
            for (uint32_t x = 0; x <= cells_; ++x) {
                const float h = heights_[index(x, z)];
                lowest = std::min(lowest, h);
                highest = std::max(highest, h);
                const float dx = (heights_[index(std::min(x + 1, cells_), z)] - heights_[index(x > 0 ? x - 1 : 0, z)]) / (cell_size_ * 2.f);
                const float dz = (heights_[index(x, std::min(z + 1, cells_))] - heights_[index(x, z > 0 ? z - 1 : 0)]) / (cell_size_ * 2.f);
                const float length = std::sqrt(dx * dx + 1.f + dz * dz);
                const float across = std::sqrt(1.f + dx * dx);
                vertices.push_back({{grid_coord(x), base - h, grid_coord(z)},
                                    {-dx / length, -1.f / length, -dz / length},
                                    {static_cast<float>(x) / static_cast<float>(cells_), static_cast<float>(z) / static_cast<float>(cells_)},
                                    {1.f / across, -dx / across, 0.f, 1.f}});
            }
        }
        velk::vector<uint32_t> indices;
        indices.reserve(static_cast<size_t>(cells_) * cells_ * 6);
        for (uint32_t z = 0; z < cells_; ++z) {
            for (uint32_t x = 0; x < cells_; ++x) {
                const auto i00 = static_cast<uint32_t>(index(x, z));
                const auto i10 = static_cast<uint32_t>(index(x + 1, z));
                const auto i01 = static_cast<uint32_t>(index(x, z + 1));
                const auto i11 = static_cast<uint32_t>(index(x + 1, z + 1));
                // +x then +z winds towards -y, which is up.
                for (uint32_t i : {i00, i10, i11, i00, i11, i01}) {
                    indices.push_back(i);
                }
            }
        }
        velk::aabb bounds{};
        bounds.position = {-half_size_, base - highest, -half_size_};
        bounds.extent = {half_size_ * 2.f, highest - lowest, half_size_ * 2.f};
        const auto vertex_count = static_cast<uint32_t>(vertices.size());
        const auto index_count = static_cast<uint32_t>(indices.size());
        auto& builder = ctx.get_mesh_builder();
        auto primitive = builder.build_primitive({kAttributes, 4}, sizeof(Vertex), vertices.data(), vertex_count, indices.data(),
                                                 index_count, velk::MeshTopology::TriangleList, bounds);
        primitive->set_rt_blas(velk::build_mesh_blas(reinterpret_cast<const float*>(vertices.data()), sizeof(Vertex), vertex_count,
                                                     indices.data(), index_count / 3));
        velk::MeshPrimitive(primitive).set_material(material);
        velk::IMeshPrimitive::Ptr primitives[] = {primitive};
        return builder.build({primitives, 1});
    }

private:
    size_t index(uint32_t x, uint32_t z) const { return static_cast<size_t>(z) * (cells_ + 1) + x; }
    float grid_coord(uint32_t i) const { return static_cast<float>(i) * cell_size_ - half_size_; }

    static float lattice(int x, int z)
    {
        auto h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(z) * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return static_cast<float>(h ^ (h >> 16)) / 4294967295.f * 2.f - 1.f;
    }

    static float noise(float x, float z)
    {
        const float fx = std::floor(x);
        const float fz = std::floor(z);
        const int ix = static_cast<int>(fx);
        const int iz = static_cast<int>(fz);
        const float tx = x - fx;
        const float tz = z - fz;
        const float ux = tx * tx * (3.f - 2.f * tx);
        const float uz = tz * tz * (3.f - 2.f * tz);
        const float a = lattice(ix, iz) + (lattice(ix + 1, iz) - lattice(ix, iz)) * ux;
        const float b = lattice(ix, iz + 1) + (lattice(ix + 1, iz + 1) - lattice(ix, iz + 1)) * ux;
        return a + (b - a) * uz;
    }

    /// Rolling ground, with ridged mountains over part of it.
    static float generate(float x, float z)
    {
        struct Octave
        {
            float wavelength;
            float amplitude;
        };
        const float rolling = noise(x / 6000.f, z / 6000.f) * 350.f + noise(x / 2400.f, z / 2400.f) * 120.f;

        const float t = std::clamp((noise(x / 16000.f + 3.7f, z / 16000.f + 8.1f) + 0.25f) / 0.6f, 0.f, 1.f);
        const float mountains = t * t * (3.f - 2.f * t);

        static constexpr Octave kRidges[] = {{7000.f, 2400.f}, {3100.f, 900.f}, {1400.f, 320.f}, {700.f, 90.f}};
        float ridges = 0.f;
        float weight = 1.f;
        float offset = 11.3f;
        for (const auto& o : kRidges) {
            float ridge = 1.f - std::abs(noise(x / o.wavelength + offset, z / o.wavelength - offset));
            ridge *= ridge;
            ridges += ridge * o.amplitude * weight;
            // Detail follows the ridges, and valleys stay smooth.
            weight = std::min(ridge * 1.2f, 1.f);
            offset += 5.9f;
        }
        return rolling + mountains * ridges;
    }

    float half_size_;
    uint32_t cells_;
    float cell_size_;
    velk::vector<float> heights_;
};

/// Builds a triangle mesh sphere with its own BLAS. The element's size does not
/// scale a mesh visual, so its vertices are in world units.
velk::IMesh::Ptr build_triangle_mesh(velk::IRenderContext& ctx, float size, uint32_t segments = 16,
                                     uint32_t rings = 8)
{
    constexpr float kPi = 3.14159265358979323846f;
    const float radius = size * 0.5f;

    velk::vector<Vertex> vertices;
    velk::vector<uint32_t> indices;

    for (uint32_t r = 0; r <= rings; ++r) {
        const float vy = static_cast<float>(r) / rings;
        const float phi = vy * kPi;
        for (uint32_t s = 0; s <= segments; ++s) {
            const float vx = static_cast<float>(s) / segments;
            const float theta = vx * kPi * 2.f;
            const float nx = std::sin(phi) * std::cos(theta);
            const float ny = std::cos(phi);
            const float nz = std::sin(phi) * std::sin(theta);
            vertices.push_back({{radius + radius * nx, radius + radius * ny, radius + radius * nz},
                                {nx, ny, nz},
                                {vx, vy},
                                {-std::sin(theta), 0.f, std::cos(theta), 1.f}});
        }
    }

    const uint32_t stride = segments + 1;
    for (uint32_t r = 0; r < rings; ++r) {
        for (uint32_t s = 0; s < segments; ++s) {
            const uint32_t i00 = r * stride + s;
            const uint32_t i10 = i00 + 1;
            const uint32_t i01 = i00 + stride;
            const uint32_t i11 = i01 + 1;
            for (uint32_t i : {i00, i01, i11, i00, i11, i10}) {
                indices.push_back(i);
            }
        }
    }

    static constexpr velk::VertexAttribute kAttributes[] = {
        {velk::VertexAttributeSemantic::Position, velk::VertexAttributeFormat::Float3, 0},
        {velk::VertexAttributeSemantic::Normal, velk::VertexAttributeFormat::Float3, 12},
        {velk::VertexAttributeSemantic::TexCoord0, velk::VertexAttributeFormat::Float2, 24},
        {velk::VertexAttributeSemantic::Tangent, velk::VertexAttributeFormat::Float4, 32},
    };

    velk::aabb bounds{};
    bounds.position = {0.f, 0.f, 0.f};
    bounds.extent = {size, size, size};

    const auto vertex_count = static_cast<uint32_t>(vertices.size());
    const auto index_count = static_cast<uint32_t>(indices.size());

    auto& builder = ctx.get_mesh_builder();
    auto primitive = builder.build_primitive({kAttributes, 4}, sizeof(Vertex), vertices.data(), vertex_count,
                                             indices.data(), index_count, velk::MeshTopology::TriangleList, bounds);
    if (!primitive) {
        return nullptr;
    }
    primitive->set_rt_blas(velk::build_mesh_blas(reinterpret_cast<const float*>(vertices.data()), sizeof(Vertex),
                                                 vertex_count, indices.data(), index_count / 3));
    velk::IMeshPrimitive::Ptr primitives[] = {primitive};
    return builder.build({primitives, 1});
}

/// Builds a lumpy rock of about 25,000 triangles standing on y = 0 and growing towards -y.
velk::IMesh::Ptr build_rock_mesh(velk::IRenderContext& ctx, float size, const velk::IMaterial::Ptr& material)
{
    constexpr float kPi = 3.14159265358979323846f;
    constexpr uint32_t kSegments = 160;
    constexpr uint32_t kRings = 80;
    static constexpr velk::VertexAttribute kAttributes[] = {
        {velk::VertexAttributeSemantic::Position, velk::VertexAttributeFormat::Float3, 0},
        {velk::VertexAttributeSemantic::Normal, velk::VertexAttributeFormat::Float3, 12},
        {velk::VertexAttributeSemantic::TexCoord0, velk::VertexAttributeFormat::Float2, 24},
        {velk::VertexAttributeSemantic::Tangent, velk::VertexAttributeFormat::Float4, 32},
    };

    const float radius = size * 0.5f;
    const auto surface = [&](float theta, float phi) {
        const velk::vec3 d{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
        const float lumps = 0.22f * std::sin(d.x * 3.1f + 0.7f) * std::cos(d.z * 2.7f)
                            + 0.11f * std::sin(d.y * 7.3f + d.x * 5.1f)
                            + 0.05f * std::sin(d.z * 17.f + 1.3f) * std::sin(d.x * 19.f + d.y * 13.f);
        const float r = radius * (0.8f + lumps);
        return velk::vec3{d.x * r, -radius + d.y * r, d.z * r};
    };

    velk::vector<Vertex> vertices;
    velk::vector<uint32_t> indices;
    for (uint32_t r = 0; r <= kRings; ++r) {
        const float phi = static_cast<float>(r) / kRings * kPi;
        for (uint32_t s = 0; s <= kSegments; ++s) {
            const float theta = static_cast<float>(s) / kSegments * kPi * 2.f;
            const velk::vec3 p = surface(theta, phi);
            const velk::vec3 from_center{p.x, p.y + radius, p.z};
            constexpr float kStep = 0.002f;
            const velk::vec3 a = surface(theta + kStep, phi);
            const velk::vec3 b = surface(theta - kStep, phi);
            const velk::vec3 c = surface(theta, std::min(phi + kStep, kPi - kStep));
            const velk::vec3 d = surface(theta, std::max(phi - kStep, kStep));
            const velk::vec3 u{a.x - b.x, a.y - b.y, a.z - b.z};
            const velk::vec3 v{c.x - d.x, c.y - d.y, c.z - d.z};
            velk::vec3 n{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
            const float towards = n.x * from_center.x + n.y * from_center.y + n.z * from_center.z;
            const float length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            const float flip = (towards < 0.f ? -1.f : 1.f) / std::max(length, 1e-9f);
            n = {n.x * flip, n.y * flip, n.z * flip};
            vertices.push_back({{p.x, p.y, p.z}, {n.x, n.y, n.z}, {0.f, 0.f}, {-std::sin(theta), 0.f, std::cos(theta), 1.f}});
        }
    }
    const auto add_triangle = [&](uint32_t i0, uint32_t i1, uint32_t i2) {
        const auto& a = vertices[i0].position;
        const auto& b = vertices[i1].position;
        const auto& c = vertices[i2].position;
        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const float nx = e1[1] * e2[2] - e1[2] * e2[1];
        const float ny = e1[2] * e2[0] - e1[0] * e2[2];
        const float nz = e1[0] * e2[1] - e1[1] * e2[0];
        if (nx == 0.f && ny == 0.f && nz == 0.f) {
            return;
        }
        // Outward, seen from the winding.
        const bool outward = nx * a[0] + ny * (a[1] + radius) + nz * a[2] > 0.f;
        indices.push_back(i0);
        indices.push_back(outward ? i1 : i2);
        indices.push_back(outward ? i2 : i1);
    };
    const uint32_t stride = kSegments + 1;
    for (uint32_t r = 0; r < kRings; ++r) {
        for (uint32_t s = 0; s < kSegments; ++s) {
            const uint32_t i00 = r * stride + s;
            add_triangle(i00, i00 + stride, i00 + stride + 1);
            add_triangle(i00, i00 + stride + 1, i00 + 1);
        }
    }

    velk::aabb bounds{};
    bounds.position = {-size * 0.7f, -size * 1.2f, -size * 0.7f};
    bounds.extent = {size * 1.4f, size * 1.4f, size * 1.4f};
    const auto vertex_count = static_cast<uint32_t>(vertices.size());
    const auto index_count = static_cast<uint32_t>(indices.size());
    auto& builder = ctx.get_mesh_builder();
    auto primitive = builder.build_primitive({kAttributes, 4}, sizeof(Vertex), vertices.data(), vertex_count, indices.data(),
                                             index_count, velk::MeshTopology::TriangleList, bounds);
    primitive->set_rt_blas(velk::build_mesh_blas(reinterpret_cast<const float*>(vertices.data()), sizeof(Vertex), vertex_count,
                                                 indices.data(), index_count / 3));
    velk::MeshPrimitive(primitive).set_material(material);
    velk::IMeshPrimitive::Ptr primitives[] = {primitive};
    return builder.build({primitives, 1});
}

/// The direction towards a directional light with this rotation, in degrees.
velk::vec3 towards_light(const velk::vec3& rotation)
{
    constexpr float kDegToRad = 3.14159265358979323846f / 180.f;
    const velk::mat4 m = velk::quat::from_euler({rotation.x * kDegToRad, rotation.y * kDegToRad, rotation.z * kDegToRad}).to_mat4();
    return {m.m[8], m.m[9], m.m[10]};
}

/// Generates a clear sky as the bytes of a Radiance HDR file, equirectangular with up at the
/// top of the image: pale at the horizon and deep blue overhead, with dim ground below it and
/// a sun with a glow around it towards @p sun.
velk::vector<uint8_t> build_sky_hdr(const velk::vec3& sun)
{
    constexpr int kWidth = 1024;
    constexpr int kHeight = 512;
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kDiscCos = 0.99978f;  // 1.2 degrees across its radius.
    constexpr float kDiscEdgeCos = 0.99990f;
    constexpr float kDiscRadiance = 40.f;
    velk::vector<uint8_t> bytes;
    char header[96];
    const int header_size = std::snprintf(header, sizeof(header), "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n", kHeight, kWidth);
    for (int i = 0; i < header_size; ++i) {
        bytes.push_back(static_cast<uint8_t>(header[i]));
    }
    for (int y = 0; y < kHeight; ++y) {
        const float up = std::cos((static_cast<float>(y) + 0.5f) / static_cast<float>(kHeight) * kPi);
        float rgb[3];
        if (up >= 0.f) {
            const float t = std::pow(up, 0.45f);
            const float horizon[3] = {1.05f, 1.10f, 1.15f};
            const float zenith[3] = {0.22f, 0.42f, 0.95f};
            for (int k = 0; k < 3; ++k) {
                rgb[k] = horizon[k] + (zenith[k] - horizon[k]) * t;
            }
        } else {
            // Haze fading down to dim ground, so the terrain's edge blends into the distance.
            const float t = std::min(-up / 0.5f, 1.f);
            const float horizon[3] = {1.05f, 1.10f, 1.15f};
            const float ground[3] = {0.22f, 0.24f, 0.18f};
            for (int k = 0; k < 3; ++k) {
                rgb[k] = horizon[k] + (ground[k] - horizon[k]) * t;
            }
        }
        const float across = std::sqrt(std::max(1.f - up * up, 0.f));
        for (int x = 0; x < kWidth; ++x) {
            const float turn = ((static_cast<float>(x) + 0.5f) / static_cast<float>(kWidth) - 0.5f) * 2.f * kPi;
            const float along = std::max(across * std::cos(turn) * sun.x - up * sun.y + across * std::sin(turn) * sun.z, 0.f);
            const float disc = std::clamp((along - kDiscCos) / (kDiscEdgeCos - kDiscCos), 0.f, 1.f);
            const float light = disc * kDiscRadiance + 0.8f * std::pow(along, 900.f) + 0.12f * std::pow(along, 40.f);
            const float pixel[3] = {rgb[0] + light, rgb[1] + light * 0.96f, rgb[2] + light * 0.88f};
            int exponent = 0;
            const float largest = std::max({pixel[0], pixel[1], pixel[2]});
            const float scale = std::frexp(largest, &exponent) * 256.f / largest;
            bytes.push_back(static_cast<uint8_t>(pixel[0] * scale));
            bytes.push_back(static_cast<uint8_t>(pixel[1] * scale));
            bytes.push_back(static_cast<uint8_t>(pixel[2] * scale));
            bytes.push_back(static_cast<uint8_t>(exponent + 128));
        }
    }
    return bytes;
}

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

    velk::IMesh::Ptr build(velk::IRenderContext& ctx, const velk::IMaterial::Ptr& bark, const velk::IMaterial::Ptr& foliage)
    {
        grow(0, {0.f, 0.f, 0.f}, {0.f, -1.f, 0.f}, shape_.trunk_length, shape_.trunk_radius);
        shade_leaves();
        velk::IMeshPrimitive::Ptr leaves = make_primitive(ctx, leaves_, leaf_indices_, foliage);
        leaves->set_merge_pieces(true);
        velk::IMeshPrimitive::Ptr primitives[] = {make_primitive(ctx, wood_, wood_indices_, bark), leaves};
        return ctx.get_mesh_builder().build({primitives, 2});
    }

private:
    float random()
    {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return static_cast<float>(state_ % 100000u) / 100000.f;
    }

    velk::vec3 random_direction()
    {
        const float z = random() * 2.f - 1.f;
        const float theta = random() * 6.2831853f;
        const float r = std::sqrt(std::max(1.f - z * z, 0.f));
        return {r * std::cos(theta), z, r * std::sin(theta)};
    }

    static velk::vec3 perpendicular(const velk::vec3& d)
    {
        const velk::vec3 axis = std::abs(d.y) < 0.9f ? velk::vec3{0.f, 1.f, 0.f} : velk::vec3{1.f, 0.f, 0.f};
        return velk::vec3::normalize(velk::vec3::cross(d, axis));
    }

    /// One branch and those growing from it.
    void grow(int level, const velk::vec3& base, const velk::vec3& direction, float length, float radius)
    {
        const int segments = level == 0 ? 6 : 3;
        const uint32_t sides = level == 0 ? 7u : level == 1 ? 5u : 3u;
        const velk::vec3 up{0.f, -1.f, 0.f};
        velk::vector<velk::vec3> points;
        velk::vector<velk::vec3> directions;
        velk::vec3 p = base;
        velk::vec3 d = direction;
        for (int s = 0; s <= segments; ++s) {
            points.push_back(p);
            directions.push_back(d);
            d = velk::vec3::normalize(d + up * (shape_.rise / static_cast<float>(segments)) + random_direction() * 0.12f);
            p = p + d * (length / static_cast<float>(segments));
        }

        // Rings along the branch, narrowing towards its end.
        const auto first_vertex = static_cast<uint32_t>(wood_.size());
        velk::vec3 u = perpendicular(directions[0]);
        for (int s = 0; s <= segments; ++s) {
            const velk::vec3& dir = directions[static_cast<size_t>(s)];
            u = velk::vec3::normalize(u - dir * velk::vec3::dot(u, dir));
            const velk::vec3 v = velk::vec3::cross(dir, u);
            const float along = static_cast<float>(s) / static_cast<float>(segments);
            const float ring_radius = radius * (1.f - 0.75f * along);
            for (uint32_t side = 0; side < sides; ++side) {
                const float theta = static_cast<float>(side) / static_cast<float>(sides) * 6.2831853f;
                const velk::vec3 n = u * std::cos(theta) + v * std::sin(theta);
                const velk::vec3 at = points[static_cast<size_t>(s)] + n * ring_radius;
                wood_.push_back({{at.x, at.y, at.z}, {n.x, n.y, n.z}, {0.f, along}, {dir.x, dir.y, dir.z, 1.f}});
            }
        }
        for (int s = 0; s < segments; ++s) {
            for (uint32_t side = 0; side < sides; ++side) {
                const uint32_t next = (side + 1) % sides;
                const uint32_t a = first_vertex + static_cast<uint32_t>(s) * sides + side;
                const uint32_t b = first_vertex + static_cast<uint32_t>(s) * sides + next;
                for (uint32_t i : {a, b + sides, a + sides, a, b, b + sides}) {
                    wood_indices_.push_back(i);
                }
            }
        }

        if (level == shape_.levels) {
            for (int leaf = 0; leaf < shape_.leaves; ++leaf) {
                const float t = 0.25f + 0.75f * random();
                const auto at = static_cast<size_t>(t * static_cast<float>(segments));
                add_leaf(points[at] + random_direction() * (length * 0.3f), directions[at]);
            }
            return;
        }

        for (int child = 0; child < shape_.children; ++child) {
            const float t = shape_.first_child + (1.f - shape_.first_child) * (static_cast<float>(child) + random() * 0.6f)
                / static_cast<float>(shape_.children);
            const float position = t * static_cast<float>(segments);
            const auto at = std::min(static_cast<size_t>(position), static_cast<size_t>(segments - 1));
            const float blend = position - static_cast<float>(at);
            const velk::vec3 from = points[at] + (points[at + 1] - points[at]) * blend;
            const velk::vec3 dir = directions[at];
            // Around the parent by the golden angle, so branches do not line up.
            const float around = static_cast<float>(child) * 2.39996f + random() * 0.5f;
            const velk::vec3 side_u = perpendicular(dir);
            const velk::vec3 side_v = velk::vec3::cross(dir, side_u);
            const velk::vec3 out = side_u * std::cos(around) + side_v * std::sin(around);
            const float angle = shape_.spread * (0.8f + 0.4f * random());
            const velk::vec3 child_direction = velk::vec3::normalize(dir * std::cos(angle) + out * std::sin(angle));
            const float scale = shape_.taper_with_height ? (1.05f - t) : (0.8f + 0.4f * random());
            grow(level + 1, from, child_direction, length * shape_.length_ratio * scale,
                 radius * (1.f - 0.75f * t) * shape_.radius_ratio);
        }
    }

    void add_leaf(const velk::vec3& at, const velk::vec3& direction)
    {
        const velk::vec3 axis = velk::vec3::normalize(direction + random_direction());
        const velk::vec3 across = velk::vec3::normalize(velk::vec3::cross(axis, random_direction())) * (shape_.leaf_width * 0.5f);
        const velk::vec3 mid = at + axis * (shape_.leaf_length * 0.5f);
        const velk::vec3 tip = at + axis * shape_.leaf_length;
        const velk::vec3 corners[] = {at, mid + across, tip, mid - across};
        const velk::vec3 normal = velk::vec3::normalize(velk::vec3::cross(across, axis));
        const auto first = static_cast<uint32_t>(leaves_.size());
        for (const auto& c : corners) {
            leaves_.push_back({{c.x, c.y, c.z}, {normal.x, normal.y, normal.z}, {1.f, 0.f}, {axis.x, axis.y, axis.z, 1.f}});
        }
        for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) {
            leaf_indices_.push_back(first + i);
        }
    }

    /// Normals lean away from the crown's middle, so it shades as one soft shape.
    void shade_leaves()
    {
        velk::vec3 center{0.f, 0.f, 0.f};
        for (const auto& leaf : leaves_) {
            center = center + velk::vec3{leaf.position[0], leaf.position[1], leaf.position[2]};
        }
        center = center * (1.f / static_cast<float>(std::max<size_t>(leaves_.size(), 1)));
        for (auto& leaf : leaves_) {
            const velk::vec3 out = velk::vec3::normalize(velk::vec3{leaf.position[0], leaf.position[1], leaf.position[2]} - center);
            const velk::vec3 own{leaf.normal[0], leaf.normal[1], leaf.normal[2]};
            const velk::vec3 n = velk::vec3::normalize(own * 0.35f + out * 0.65f);
            leaf.normal[0] = n.x;
            leaf.normal[1] = n.y;
            leaf.normal[2] = n.z;
        }
    }

public:
    static velk::IMeshPrimitive::Ptr make_primitive(velk::IRenderContext& ctx, const velk::vector<Vertex>& vertices,
                                                    const velk::vector<uint32_t>& indices, const velk::IMaterial::Ptr& material)
    {
        static constexpr velk::VertexAttribute kAttributes[] = {
            {velk::VertexAttributeSemantic::Position, velk::VertexAttributeFormat::Float3, 0},
            {velk::VertexAttributeSemantic::Normal, velk::VertexAttributeFormat::Float3, 12},
            {velk::VertexAttributeSemantic::TexCoord0, velk::VertexAttributeFormat::Float2, 24},
            {velk::VertexAttributeSemantic::Tangent, velk::VertexAttributeFormat::Float4, 32},
        };
        velk::vec3 lo{vertices[0].position[0], vertices[0].position[1], vertices[0].position[2]};
        velk::vec3 hi = lo;
        for (const auto& vertex : vertices) {
            lo.x = std::min(lo.x, vertex.position[0]);
            lo.y = std::min(lo.y, vertex.position[1]);
            lo.z = std::min(lo.z, vertex.position[2]);
            hi.x = std::max(hi.x, vertex.position[0]);
            hi.y = std::max(hi.y, vertex.position[1]);
            hi.z = std::max(hi.z, vertex.position[2]);
        }
        velk::aabb bounds{};
        bounds.position = lo;
        bounds.extent = {hi.x - lo.x, hi.y - lo.y, hi.z - lo.z};
        const auto vertex_count = static_cast<uint32_t>(vertices.size());
        const auto index_count = static_cast<uint32_t>(indices.size());
        auto primitive = ctx.get_mesh_builder().build_primitive({kAttributes, 4}, sizeof(Vertex), vertices.data(), vertex_count,
                                                                indices.data(), index_count, velk::MeshTopology::TriangleList, bounds);
        primitive->set_rt_blas(velk::build_mesh_blas(reinterpret_cast<const float*>(vertices.data()), sizeof(Vertex), vertex_count,
                                                     indices.data(), index_count / 3));
        velk::MeshPrimitive(primitive).set_material(material);
        return primitive;
    }

private:
    TreeShape shape_;
    uint32_t state_;
    velk::vector<Vertex> wood_;
    velk::vector<uint32_t> wood_indices_;
    velk::vector<Vertex> leaves_;
    velk::vector<uint32_t> leaf_indices_;
};

/// Builds a square of grass blades @p size across on y = 0, growing towards -y. A blade's
/// first uv is its height, from 0 to 1, for the vertex shader to move its tip the most.
velk::IMesh::Ptr build_grass_patch(velk::IRenderContext& ctx, float size, const velk::IMaterial::Ptr& material)
{
    constexpr int kBlades = 200;
    constexpr int kSegments = 2;
    uint32_t state = 2166136261u;
    const auto random = [&state]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(state % 100000u) / 100000.f;
    };
    const velk::vec3 up{0.f, -1.f, 0.f};
    velk::vector<Vertex> vertices;
    velk::vector<uint32_t> indices;
    for (int blade = 0; blade < kBlades; ++blade) {
        const velk::vec3 base{(random() - 0.5f) * size, 0.f, (random() - 0.5f) * size};
        const float height = 3.f + random() * 2.f;
        const float turn = random() * 6.2831853f;
        const velk::vec3 lean{std::cos(turn), 0.f, std::sin(turn)};
        const velk::vec3 side{-lean.z * 0.28f, 0.f, lean.x * 0.28f};
        const velk::vec3 normal = velk::vec3::normalize(up * 0.8f + lean * 0.6f);
        const auto first = static_cast<uint32_t>(vertices.size());
        for (int s = 0; s <= kSegments; ++s) {
            const float f = static_cast<float>(s) / static_cast<float>(kSegments + 1);
            const velk::vec3 at = base + up * (f * height) + lean * (f * f * height * 0.5f);
            const velk::vec3 half = side * (1.f - f);
            for (const velk::vec3& p : {at - half, at + half}) {
                vertices.push_back({{p.x, p.y, p.z}, {normal.x, normal.y, normal.z}, {f, 0.f}, {side.x, side.y, side.z, 1.f}});
            }
        }
        const velk::vec3 tip = base + up * height + lean * (height * 0.5f);
        vertices.push_back({{tip.x, tip.y, tip.z}, {normal.x, normal.y, normal.z}, {1.f, 0.f}, {side.x, side.y, side.z, 1.f}});
        for (uint32_t s = 0; s < kSegments; ++s) {
            const uint32_t a = first + s * 2u;
            for (uint32_t i : {a, a + 1u, a + 3u, a, a + 3u, a + 2u}) {
                indices.push_back(i);
            }
        }
        const uint32_t top = first + kSegments * 2u;
        for (uint32_t i : {top, top + 1u, top + 2u}) {
            indices.push_back(i);
        }
    }
    velk::IMeshPrimitive::Ptr primitives[] = {TreeGrower::make_primitive(ctx, vertices, indices, material)};
    primitives[0]->set_merge_pieces(true);
    return ctx.get_mesh_builder().build({primitives, 1});
}

/// Builds a low-poly tree standing on y = 0 and growing towards -y: a trunk and a
/// crown, as a primitive each.
velk::IMesh::Ptr build_tree_mesh(velk::IRenderContext& ctx, float height, const velk::IMaterial::Ptr& trunk,
                                 const velk::IMaterial::Ptr& crown, uint32_t sides = 6, uint32_t rings = 1)
{
    constexpr float kPi = 3.14159265358979323846f;

    static constexpr velk::VertexAttribute kAttributes[] = {
        {velk::VertexAttributeSemantic::Position, velk::VertexAttributeFormat::Float3, 0},
        {velk::VertexAttributeSemantic::Normal, velk::VertexAttributeFormat::Float3, 12},
        {velk::VertexAttributeSemantic::TexCoord0, velk::VertexAttributeFormat::Float2, 24},
        {velk::VertexAttributeSemantic::Tangent, velk::VertexAttributeFormat::Float4, 32},
    };

    auto& builder = ctx.get_mesh_builder();
    const auto build_cone = [&](float bottom_y, float top_y, float bottom_radius, float top_radius,
                                const velk::IMaterial::Ptr& material) -> velk::IMeshPrimitive::Ptr {
        velk::vector<Vertex> vertices;
        velk::vector<uint32_t> indices;
        for (uint32_t s = 0; s < sides; ++s) {
            const float theta = static_cast<float>(s) / static_cast<float>(sides) * kPi * 2.f;
            const float c = std::cos(theta);
            const float z = std::sin(theta);
            for (uint32_t r = 0; r <= rings; ++r) {
                const float f = static_cast<float>(r) / static_cast<float>(rings);
                const float radius = bottom_radius + (top_radius - bottom_radius) * f;
                vertices.push_back({{c * radius, bottom_y + (top_y - bottom_y) * f, z * radius},
                                    {c, -0.5f * f, z},
                                    {0.f, f},
                                    {-z, 0.f, c, 1.f}});
            }
        }
        for (uint32_t s = 0; s < sides; ++s) {
            const uint32_t next = (s + 1) % sides;
            for (uint32_t r = 0; r < rings; ++r) {
                const uint32_t a = s * (rings + 1) + r;
                const uint32_t b = next * (rings + 1) + r;
                for (uint32_t i : {a, b + 1, a + 1, a, b, b + 1}) {
                    indices.push_back(i);
                }
            }
        }
        velk::vec3 lo{-bottom_radius, std::min(bottom_y, top_y), -bottom_radius};
        velk::aabb bounds{};
        bounds.position = lo;
        bounds.extent = {bottom_radius * 2.f, std::abs(top_y - bottom_y), bottom_radius * 2.f};
        const auto vertex_count = static_cast<uint32_t>(vertices.size());
        const auto index_count = static_cast<uint32_t>(indices.size());
        auto primitive = builder.build_primitive({kAttributes, 4}, sizeof(Vertex), vertices.data(), vertex_count,
                                                 indices.data(), index_count, velk::MeshTopology::TriangleList, bounds);
        if (primitive) {
            primitive->set_rt_blas(velk::build_mesh_blas(reinterpret_cast<const float*>(vertices.data()), sizeof(Vertex),
                                                         vertex_count, indices.data(), index_count / 3));
            velk::MeshPrimitive(primitive).set_material(material);
        }
        return primitive;
    };

    velk::IMeshPrimitive::Ptr primitives[] = {build_cone(0.f, -height * 0.4f, height * 0.06f, height * 0.04f, trunk),
                                              build_cone(-height * 0.25f, -height, height * 0.25f, height * 0.01f, crown)};
    return builder.build({primitives, 2});
}

/// Builds a box standing on y = 0 and growing towards -y, as one primitive.
velk::IMesh::Ptr build_block_mesh(velk::IRenderContext& ctx, float width, float height, const velk::IMaterial::Ptr& material)
{
    static constexpr velk::VertexAttribute kAttributes[] = {
        {velk::VertexAttributeSemantic::Position, velk::VertexAttributeFormat::Float3, 0},
        {velk::VertexAttributeSemantic::Normal, velk::VertexAttributeFormat::Float3, 12},
        {velk::VertexAttributeSemantic::TexCoord0, velk::VertexAttributeFormat::Float2, 24},
        {velk::VertexAttributeSemantic::Tangent, velk::VertexAttributeFormat::Float4, 32},
    };
    struct Face
    {
        velk::vec3 normal;
        velk::vec3 u;
        velk::vec3 v;
    };
    // u x v = normal, so each face winds outward.
    const Face faces[] = {
        {{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}},  {{-1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 1.f, 0.f}},
        {{0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {1.f, 0.f, 0.f}},  {{0.f, -1.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}},
        {{0.f, 0.f, 1.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}},  {{0.f, 0.f, -1.f}, {0.f, 1.f, 0.f}, {1.f, 0.f, 0.f}},
    };
    const velk::vec3 half{width * 0.5f, height * 0.5f, width * 0.5f};
    const velk::vec3 center{0.f, -height * 0.5f, 0.f};
    const auto scaled = [&](const velk::vec3& d) { return velk::vec3{d.x * half.x, d.y * half.y, d.z * half.z}; };
    velk::vector<Vertex> vertices;
    velk::vector<uint32_t> indices;
    for (const auto& face : faces) {
        const auto base = static_cast<uint32_t>(vertices.size());
        const velk::vec3 c = center + scaled(face.normal);
        const velk::vec3 u = scaled(face.u);
        const velk::vec3 v = scaled(face.v);
        const velk::vec3 corners[] = {c - u - v, c + u - v, c + u + v, c - u + v};
        for (const auto& corner : corners) {
            vertices.push_back({{corner.x, corner.y, corner.z},
                                {face.normal.x, face.normal.y, face.normal.z},
                                {0.f, 0.f},
                                {face.u.x, face.u.y, face.u.z, 1.f}});
        }
        for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) {
            indices.push_back(base + i);
        }
    }
    velk::aabb bounds{};
    bounds.position = {-half.x, -height, -half.z};
    bounds.extent = {width, height, width};
    const auto vertex_count = static_cast<uint32_t>(vertices.size());
    const auto index_count = static_cast<uint32_t>(indices.size());
    auto& builder = ctx.get_mesh_builder();
    auto primitive = builder.build_primitive({kAttributes, 4}, sizeof(Vertex), vertices.data(), vertex_count, indices.data(),
                                             index_count, velk::MeshTopology::TriangleList, bounds);
    primitive->set_rt_blas(velk::build_mesh_blas(reinterpret_cast<const float*>(vertices.data()), sizeof(Vertex), vertex_count,
                                                 indices.data(), index_count / 3));
    velk::MeshPrimitive(primitive).set_material(material);
    velk::IMeshPrimitive::Ptr primitives[] = {primitive};
    return builder.build({primitives, 1});
}

/// A position that looks random but follows from @p i, so runs are comparable.
float forest_jitter(long i, float seed)
{
    const float v = std::sin(static_cast<float>(i) * seed) * 43758.5453f;
    return v - std::floor(v) - 0.5f;
}

/// Median and worst GPU time for one pass across the measured frames.
struct PassSamples
{
    const char* label;
    velk::vector<float> ms;
};

void record_gpu_timings(velk::vector<PassSamples>& passes, velk::array_view<const velk::GpuPassTiming> timings)
{
    for (const auto& timing : timings) {
        PassSamples* found = nullptr;
        for (auto& pass : passes) {
            if (std::strcmp(pass.label, timing.label) == 0) {
                found = &pass;
                break;
            }
        }
        if (!found) {
            passes.push_back({timing.label, {}});
            found = &passes.back();
        }
        found->ms.push_back(timing.ms);
    }
}

/// Where a flyby's camera holds for a section. It then moves to the next stop over another.
struct FlybyStop
{
    const char* name;
    const char* move_name;
    float distance;
    float pitch;
};

constexpr long kFlybyHoldFrames = 300;
constexpr long kFlybyMoveFrames = 240;

void print_gpu_timings(velk::vector<PassSamples>& passes)
{
    for (auto& pass : passes) {
        if (pass.ms.empty()) {
            continue;
        }
        std::sort(pass.ms.begin(), pass.ms.end());
        VELK_LOG(I, "scale: gpu %-24s med=%7.3f ms  max=%7.3f ms", pass.label,
                 static_cast<double>(pass.ms[pass.ms.size() / 2]), static_cast<double>(pass.ms.back()));
    }
}

} // namespace

int run_gi_test(const char* test, long frames);

int main(int argc, char* argv[])
{
    if (argc > 2 && std::strcmp(argv[1], "gitest") == 0) {
        return run_gi_test(argv[2], argc > 3 ? std::strtol(argv[3], nullptr, 10) : 240);
    }

    const long element_count = argc > 1 ? std::max(0L, std::strtol(argv[1], nullptr, 10)) : 1000;
    const long mover_count = std::min(element_count, argc > 2 ? std::max(0L, std::strtol(argv[2], nullptr, 10)) : 10L);
    const long frame_limit = argc > 3 ? std::strtol(argv[3], nullptr, 10) : 0;
    const bool rotate_only = argc > 4 && std::strcmp(argv[4], "rotate") == 0;
    const bool group_movers = argc > 4 && std::strcmp(argv[4], "group") == 0;
    const bool escape_movers = argc > 4 && std::strcmp(argv[4], "escape") == 0;
    const bool burst_movers = argc > 4 && std::strcmp(argv[4], "burst") == 0;
    const bool churn_elements = argc > 4 && std::strcmp(argv[4], "churn") == 0;
    const bool recolor_rects = argc > 4 && std::strcmp(argv[4], "recolor") == 0;
    const bool scatter_movers = argc > 4 && std::strcmp(argv[4], "scatter") == 0;
    const bool deform_movers = scatter_movers || (argc > 4 && std::strcmp(argv[4], "deform") == 0);
    const bool forest_shift = argc > 4 && std::strcmp(argv[4], "forest_shift") == 0;
    const bool forest_churn = argc > 4 && std::strcmp(argv[4], "forest_churn") == 0;
    const bool forest_cluster = argc > 4 && std::strcmp(argv[4], "forest_cluster") == 0;
    const bool nature = argc > 4 && std::strcmp(argv[4], "nature") == 0;
    const bool forest_instances = forest_shift || forest_churn || forest_cluster || nature
        || (argc > 4 && std::strcmp(argv[4], "forest") == 0);
    const bool forest_elements = argc > 4 && std::strcmp(argv[4], "forest_elements") == 0;
    const bool forest = forest_instances || forest_elements;
    bool unlimited = false;
    bool ray_traced = false;
    bool reference = false;
    bool with_text = false;
    bool with_overlay = false;
    bool draw_near = false;
    bool species = false;
    bool wind = false;
    bool gpu_deform = false;
    bool outlier = false;
    bool mixed = false;
    bool flyby = false;
    bool heavy = false;
    bool real_trees = false;
    uint32_t residency_mb = 0;
    bool low_sun = false;
    bool dense = false;
    bool blocks = false;
    bool rocks = false;
    int hold_stop = -1;
    float hold_yaw = 60.f;
    float hold_distance = 0.f;
    float hold_pitch = 0.f;
    bool has_view = false;
    float view[6] = {};
    float leaf_density = 1.f;
    bool no_shadow = false;
    bool no_wind = false;
    bool lod_test = false;
    int lod_test_tree = 0;
    bool no_sky = false;
    int kWidth = 1280;
    int kHeight = 720;
    bool no_tour = false;
    float tour_along = 0.f;
    float gi_cell = 0.f;
    long grass_count = 1000000;
    bool ray_wind = true;
    bool flat_terrain = false;
    float sun_angle = 0.f;
    for (int i = 5; i < argc; ++i) {
        gpu_deform = gpu_deform || std::strcmp(argv[i], "gpu") == 0;
        outlier = outlier || std::strcmp(argv[i], "outlier") == 0;
        mixed = mixed || std::strcmp(argv[i], "mixed") == 0;
        flyby = flyby || std::strcmp(argv[i], "flyby") == 0;
        heavy = heavy || std::strcmp(argv[i], "heavy") == 0;
        real_trees = real_trees || std::strcmp(argv[i], "real") == 0;
        low_sun = low_sun || std::strcmp(argv[i], "lowsun") == 0;
        dense = dense || std::strcmp(argv[i], "dense") == 0;
        blocks = blocks || std::strcmp(argv[i], "blocks") == 0;
        rocks = rocks || std::strcmp(argv[i], "rocks") == 0;
        if (std::strcmp(argv[i], "across") == 0) hold_stop = 1;
        if (std::strcmp(argv[i], "top") == 0) hold_stop = 2;
        if (std::strncmp(argv[i], "yaw=", 4) == 0) hold_yaw = static_cast<float>(std::atof(argv[i] + 4));
        if (std::strncmp(argv[i], "distance=", 9) == 0) hold_distance = static_cast<float>(std::atof(argv[i] + 9));
        if (std::strncmp(argv[i], "pitch=", 6) == 0) hold_pitch = static_cast<float>(std::atof(argv[i] + 6));
        if (std::strncmp(argv[i], "budget=", 7) == 0) residency_mb = static_cast<uint32_t>(std::atoi(argv[i] + 7));
        no_shadow = no_shadow || std::strcmp(argv[i], "noshadow") == 0;
        no_wind = no_wind || std::strcmp(argv[i], "nowind") == 0;
        if (std::strncmp(argv[i], "lodtest=", 8) == 0) {
            lod_test = nature;
            lod_test_tree = std::clamp(std::atoi(argv[i] + 8), 0, 2);
        }
        no_sky = no_sky || std::strcmp(argv[i], "nosky") == 0;
        if (std::strncmp(argv[i], "size=", 5) == 0) std::sscanf(argv[i] + 5, "%dx%d", &kWidth, &kHeight);
        no_tour = no_tour || std::strcmp(argv[i], "notour") == 0;
        if (std::strncmp(argv[i], "tour=", 5) == 0) tour_along = static_cast<float>(std::atof(argv[i] + 5));
        if (std::strncmp(argv[i], "gicell=", 7) == 0) gi_cell = static_cast<float>(std::atof(argv[i] + 7));
        if (std::strncmp(argv[i], "grass=", 6) == 0) grass_count = std::atol(argv[i] + 6);
        ray_wind = ray_wind && std::strcmp(argv[i], "noraywind") != 0;
        flat_terrain = flat_terrain || std::strcmp(argv[i], "flat") == 0;
        if (std::strncmp(argv[i], "sun=", 4) == 0) sun_angle = static_cast<float>(std::atof(argv[i] + 4));
        if (std::strncmp(argv[i], "leaves=", 7) == 0) leaf_density = static_cast<float>(std::atof(argv[i] + 7));
        if (std::strncmp(argv[i], "view=", 5) == 0) {
            has_view = std::sscanf(argv[i] + 5, "%f,%f,%f,%f,%f,%f", &view[0], &view[1], &view[2], &view[3], &view[4], &view[5]) == 6;
        }
        wind = wind || std::strcmp(argv[i], "wind") == 0;
        species = species || std::strcmp(argv[i], "species") == 0;
        draw_near = draw_near || std::strcmp(argv[i], "near") == 0;
        with_overlay = with_overlay || std::strcmp(argv[i], "overlay") == 0;
        unlimited = unlimited || std::strcmp(argv[i], "unlimited") == 0;
        ray_traced = ray_traced || std::strcmp(argv[i], "rt") == 0;
        reference = reference || std::strcmp(argv[i], "reference") == 0;
        with_text = with_text || std::strcmp(argv[i], "text") == 0;
    }

    if (lod_test) {
        no_wind = true;
        no_tour = true;
        flat_terrain = true;
        grass_count = 0;
        sun_angle = 40.f;
        has_view = true;
        view[0] = 0.f;
        view[1] = kFloorY - 40.f;
        view[2] = 0.f;
        view[3] = 0.f;
        view[4] = -1.f;
        view[5] = 1.f;
    }
    wind = wind || (nature && !no_wind);

    VELK_LOG(I, "scale: %ld elements, %ld movers (%s)", element_count, mover_count, argc > 4 ? argv[4] : "translate");


    velk::ApplicationConfig config;

    velk::WindowConfig wc;
    wc.width = kWidth;
    wc.height = kHeight;
    wc.title = "velk-ui scale";
    wc.depth = velk::DepthFormat::Default;
    wc.color_format = velk::SurfaceColorFormat::RGBA16F;
    if (unlimited) {
        wc.update_rate = velk::UpdateRate::Unlimited;
    }

    auto app = velk::create_app(config);
    auto window = app.create_window(wc);
    if (!window) {
        VELK_LOG(E, "scale: failed to create window");
        return 1;
    }

    auto ctx = app.render_context();
    auto render_context = static_cast<velk::IRenderContext::Ptr>(ctx);
    if (!render_context) {
        VELK_LOG(E, "scale: no render context");
        return 1;
    }
    auto backend = render_context->backend();
    if (backend) {
        backend->set_gpu_timing_enabled(true);
    }

    auto scene = velk::create_scene();
    scene.set_geometry(velk::aabb::from_size({static_cast<float>(kWidth), static_cast<float>(kHeight)}));

    auto root = velk::create_element();
    auto camera = velk::create_element();
    scene.set_root(root);
    scene.add(root, camera);

    const long columns = std::max(1L, static_cast<long>(std::ceil(std::sqrt(static_cast<double>(element_count)))));
    constexpr float kTerrainExtent = 30000.f;
    const float tree_spacing = nature ? kTerrainExtent / static_cast<float>(columns) : dense ? 30.f : kElementSpacing;
    const float extent = static_cast<float>(columns) * (forest ? tree_spacing : kElementSpacing);

    constexpr uint32_t kTerrainCells = 256;
    const Terrain terrain(nature ? extent * 0.5f + 400.f : 1.f, nature ? kTerrainCells : 1u, flat_terrain ? 0.f : 1.f);
    const auto ground_y = [&](float x, float z) { return nature ? kFloorY - terrain.height_at(x, z) : kFloorY; };

    auto cam_trait = velk::trait::render::create_camera();
    camera.add_trait(cam_trait);
    velk::Camera cam(cam_trait);
    cam.set_projection(velk::Projection::Perspective);
    cam.set_fov(45.f);
    cam.set_gi_cell_size(gi_cell);
    cam.add_render_path(reference ? velk::path::create_reference() : ray_traced ? velk::path::create_rt() : velk::path::create_deferred());

    auto post = velk::pp::create_post_process();
    post.add(velk::pp::create_tonemap());
    cam.add_post_process(post);

    const velk::vec3 sun_rotation{sun_angle > 0.f ? sun_angle : low_sun || nature ? 15.f : 35.f, 25.f, 0.f};

    if (nature && !no_sky) {
        const velk::vector<uint8_t> sky = build_sky_hdr(towards_light(sun_rotation));
        auto protocol = velk::instance().resource_store().find_protocol("memory");
        if (auto* memory = interface_cast<velk::IMemoryProtocol>(protocol.get())) {
            memory->add_file("scale_sky.hdr", sky.data(), sky.size());
            cam.set_environment(velk::ui::load_environment("env:memory://scale_sky.hdr"));
        }
    }

    // A forest is viewed from close up, at its center.
    const float distance = nature ? 16000.f : forest ? 420.f : std::max(extent * 1.3f, 800.f);
    auto orbit = velk::trait::transform::create_orbit();
    auto focus_trs = velk::trait::transform::create_trs();
    if (forest) {
        auto focus = velk::create_element();
        focus_trs.set_translate({0.f, ground_y(0.f, 0.f) - 30.f, 0.f});
        focus.add_trait(focus_trs);
        scene.add(root, focus);
        orbit.set_target(focus);
        orbit.set_pitch(nature ? -24.f : -18.f);
    } else {
        orbit.set_target(root);
        orbit.set_pitch(-30.f);
    }
    orbit.set_distance(distance);
    if (nature && hold_stop < 0) {
        // Where the same orbit would put the camera, with the focus there instead.
        constexpr float kStartPitch = -24.f * 3.14159265f / 180.f;
        focus_trs.set_translate({0.f, ground_y(0.f, 0.f) - 30.f + distance * std::sin(kStartPitch), distance * std::cos(kStartPitch)});
        orbit.set_distance(1.f);
    }
    if (has_view && forest) {
        focus_trs.set_translate({view[0], view[1], view[2]});
        orbit.set_yaw(view[3]);
        orbit.set_pitch(view[4]);
        orbit.set_distance(view[5]);
    }
    camera.add_trait(orbit);
    camera.find_trait<velk::ICamera>()->far_clip().set_value(std::max(distance, extent) * 10.f);
    if (nature) {
        velk::instance().type_registry().register_type<TerrainMaterial>();
        auto ground = velk::create_element();
        auto visual = velk::trait::visual::create_mesh();
        velk::Material material(velk::instance().create<velk::IObject>(kTerrainMaterialClass));
        // The options bring depth testing and opaque drawing, which lights it.
        material.options();
        visual.set_mesh(velk::Mesh(terrain.build_mesh(*render_context, kFloorY, material)));
        ground.add_trait(visual);
        scene.add(root, ground);
    } else {
        // Floor.
        const float half = extent * 0.5f + 400.f;
        auto floor = velk::create_element();
        auto trs = velk::trait::transform::create_trs();
        trs.set_translate({-half, kFloorY, -half});
        trs.set_rotation({90.f, 0.f, 0.f});
        auto visual = velk::ui::trait::visual::create_rect();
        visual.set_paint(velk::material::create_standard(velk::color{0.45f, 0.42f, 0.40f, 1.f}, 0.05f, 0.65f));
        floor.add_trait(velk::ui::trait::layout::create_fixed_size(velk::dim::px(half * 2.f), velk::dim::px(half * 2.f)));
        floor.add_trait(trs);
        floor.add_trait(visual);
        scene.add(root, floor);
    }

    // Sun.
    {
        auto light = velk::trait::render::create_directional_light(velk::color{1.f, 0.96f, 0.88f, 1.f}, 2.5f);
        if (!no_shadow) {
            light.add_technique(velk::technique::create_rt_shadow());
        }
        auto sun = velk::create_element();
        auto trs = velk::trait::transform::create_trs();
        trs.set_rotation(sun_rotation);
        sun.add_trait(trs);
        sun.add_trait(light);
        scene.add(root, sun);
    }

    // Grid of mixed visuals.
    auto mesh_material = velk::material::create_standard(velk::color{0.75f, 0.35f, 0.25f, 1.f}, 0.f, 0.5f);
    auto rect_material = velk::material::create_standard(velk::color{0.3f, 0.6f, 0.85f, 1.f}, 0.f, 0.4f);
    auto cube_material = velk::material::create_standard(velk::color{0.9f, 0.7f, 0.35f, 1.f}, 0.f, 0.5f);
    auto sphere_material = velk::material::create_standard(velk::color{0.4f, 0.75f, 0.45f, 1.f}, 0.f, 0.3f);

    velk::Mesh triangle_mesh(build_triangle_mesh(*render_context, kElementSize));
    triangle_mesh.set_material(0, mesh_material);
    velk::Mesh cube_mesh(ctx.build_cube());
    cube_mesh.set_material(0, cube_material);
    velk::Mesh sphere_mesh(ctx.build_sphere());
    sphere_mesh.set_material(0, sphere_material);

    struct Mover
    {
        velk::Trs trs;
        velk::vec3 base;
        float phase;
    };
    velk::vector<Mover> movers;
    movers.reserve(static_cast<size_t>(mover_count));
    velk::vector<velk::ui::RectVisual> rects;
    velk::vector<IRipple::Ptr> ripples;
    velk::vector<velk::ComputeMeshDeformer> gpu_ripples;

    velk::Mesh deform_source;
    if (deform_movers) {
        velk::instance().type_registry().register_type<Ripple>();
        deform_source = velk::Mesh(build_triangle_mesh(*render_context, kElementSize, 256, 128));
        deform_source.set_material(0, mesh_material);
    }

    const auto size_px = velk::dim::px(kElementSize);
    const float grid_start = -extent * 0.5f;
    long next_mover = 0;

    // Trees stand on the ground on a jittered grid.
    constexpr float kTreeHeight = 60.f;
    const auto tree_position = [&](long i) {
        const float x = grid_start + (static_cast<float>(i % columns) + 0.8f * forest_jitter(i, 12.9898f)) * tree_spacing;
        const float z = grid_start + (static_cast<float>(i / columns) + 0.8f * forest_jitter(i, 78.233f)) * tree_spacing;
        return velk::vec3{x, ground_y(x, z), z};
    };
    velk::InstancedMeshVisual forest_visual;
    velk::Trs forest_trs;
    uint32_t forest_extra_slot = 0xFFFFFFFFu;
    if (nature && element_count > 0) {
        velk::instance().type_registry().register_type<TreeMaterial>();
        const auto tree_material = [&](const velk::color& c, float roughness, bool double_sided) {
            auto material = velk::instance().create<ITreeMaterial>(TreeMaterial::static_class_id());
            material->albedo().set_value(c);
            material->roughness().set_value(roughness);
            material->wind().set_value(!no_wind);
            velk::Material options(interface_pointer_cast<velk::IObject>(material));
            options.options().set_double_sided(double_sided);
            return interface_pointer_cast<velk::IMaterial>(material);
        };
        const auto foliage = [&](const velk::color& c) { return tree_material(c, 0.75f, true); };
        const auto bark = [&](const velk::color& c) { return tree_material(c, 0.9f, false); };
        //                         seed trunk radius lvl kids first spread ratio radius rise  taper  leaves leaf sizes
        TreeShape oak_shape{ 11u, 22.f, 2.4f,  3,  5,   0.5f, 0.85f, 0.62f, 0.62f, 0.35f, false, 26,  3.0f, 2.2f};
        TreeShape birch_shape{23u, 34.f, 1.3f, 3,  4,   0.4f, 0.55f, 0.58f, 0.60f, -0.15f, false, 22, 2.2f, 1.6f};
        TreeShape pine_shape{37u, 78.f, 1.9f,  2,  9,   0.18f, 1.25f, 0.34f, 0.5f, 0.05f, true,  20,  3.4f, 0.8f};
        // Fewer, larger leaves keep a crown's area.
        for (TreeShape* shape : {&oak_shape, &birch_shape, &pine_shape}) {
            shape->leaves = std::max(1, static_cast<int>(static_cast<float>(shape->leaves) * leaf_density + 0.5f));
            shape->leaf_length /= std::sqrt(leaf_density);
            shape->leaf_width /= std::sqrt(leaf_density);
        }
        const velk::IMesh::Ptr oak_mesh = TreeGrower(oak_shape).build(*render_context, bark(velk::color{0.24f, 0.17f, 0.11f, 1.f}), foliage(velk::color{0.11f, 0.26f, 0.05f, 1.f}));
        const velk::IMesh::Ptr birch_mesh = TreeGrower(birch_shape).build(*render_context, bark(velk::color{0.58f, 0.55f, 0.50f, 1.f}), foliage(velk::color{0.26f, 0.38f, 0.08f, 1.f}));
        const velk::IMesh::Ptr pine_mesh = TreeGrower(pine_shape).build(*render_context, bark(velk::color{0.20f, 0.14f, 0.10f, 1.f}), foliage(velk::color{0.04f, 0.15f, 0.07f, 1.f}));
        velk::Mesh oak(oak_mesh);
        velk::Mesh birch(birch_mesh);
        velk::Mesh pine(pine_mesh);
        float tree_reach = 0.f;
        for (const auto& mesh : {oak_mesh, birch_mesh, pine_mesh}) {
            const velk::aabb bounds = mesh->get_bounds();
            const velk::vec3 lo = bounds.min();
            const velk::vec3 hi = bounds.max();
            // The corner of the box furthest from the origin, whichever sides make it up.
            const velk::vec3 far_corner{std::max(std::abs(lo.x), std::abs(hi.x)), std::max(std::abs(lo.y), std::abs(hi.y)),
                                        std::max(std::abs(lo.z), std::abs(hi.z))};
            tree_reach = std::max(tree_reach, velk::vec3::length(far_corner));
        }

        // Conifers higher up, none on steep ground or above the tree line. Each has its own
        // turn and size.
        constexpr float kTreeLine = 1300.f;
        constexpr float kConiferLine = 500.f;
        constexpr float kMaxSlope = 0.55f;
        velk::vector<velk::InstanceTransform> per_mesh[3];
        if (lod_test) {
            // Spread across the view, each half as far again as the last.
            float away = 100.f;
            for (int i = 0; i < 10; ++i) {
                const float across = (static_cast<float>(i) - 4.5f) * 0.075f * away;
                per_mesh[lod_test_tree].push_back(velk::InstanceTransform{{{1.f, 0.f, 0.f, across}, {0.f, 1.f, 0.f, kFloorY}, {0.f, 0.f, 1.f, -away}}});
                away *= 1.5f;
            }
        }
        for (long i = 0; i < (lod_test ? 0L : element_count); ++i) {
            const velk::vec3 at = tree_position(i);
            const float height = terrain.height_at(at.x, at.z);
            const float dx = terrain.height_at(at.x + 40.f, at.z) - terrain.height_at(at.x - 40.f, at.z);
            const float dz = terrain.height_at(at.x, at.z + 40.f) - terrain.height_at(at.x, at.z - 40.f);
            if (height > kTreeLine || std::sqrt(dx * dx + dz * dz) / 80.f > kMaxSlope) {
                continue;
            }
            const float pick = forest_jitter(i, 41.17f) + 0.5f;
            const float conifer_chance = std::clamp((height - kConiferLine + 300.f) / 600.f, 0.f, 1.f);
            const int species = pick < conifer_chance ? 2 : (forest_jitter(i, 7.31f) > 0.f ? 0 : 1);
            const float yaw = (forest_jitter(i, 91.7f) + 0.5f) * 6.2831853f;
            const float size = 0.75f + (forest_jitter(i, 57.3f) + 0.5f) * 0.6f;
            const float c = std::cos(yaw) * size;
            const float s = std::sin(yaw) * size;
            // Sunk a little, so a trunk on a slope has no gap under it.
            per_mesh[species].push_back(velk::InstanceTransform{{{c, 0.f, s, at.x}, {0.f, size, 0.f, at.y + 1.5f}, {-s, 0.f, c, at.z}}});
        }
        forest_visual = velk::trait::visual::create_instanced_mesh();
        if (real_trees) {
            forest_visual.set_far_form(false);
        }
        if (wind) {
            // The whole tree leans the same way in the world, whatever its turn. The bound is
            // the largest tree's reach at its largest size, by the most the angle changes it.
            constexpr float kMaxLeanChange = 1.41422f * 0.05f;
            constexpr float kMaxTreeSize = 1.35f;
            forest_visual.set_motion(R"(
mat4 velk_instance_motion(mat4 rest, uint slot, vec4 params)
{
    vec3 p = rest[3].xyz;
    float gust = sin(params.x * 0.9 + p.x * 0.004 + p.z * 0.003);
    float angle = 0.02 + 0.02 * gust + 0.008 * sin(params.x * 2.3 + float(slot) * 1.7);
    float c = cos(angle);
    float s = sin(angle);
    mat3 lean = mat3(c, s, 0.0, -s, c, 0.0, 0.0, 0.0, 1.0);
    return mat4(vec4(lean * rest[0].xyz, 0.0), vec4(lean * rest[1].xyz, 0.0), vec4(lean * rest[2].xyz, 0.0), rest[3]);
}
)", tree_reach * kMaxTreeSize * kMaxLeanChange);
            if (ray_wind) {
                // The bend in the trees' vertex shader, without the leaves' own motion.
                forest_visual.set_ray_motion(
                    "vec3(sin(t * 1.3 + dot(at.xz, vec2(0.013, 0.017)) + p.y * 0.05) * length(p.xz) * 0.02, "
                    "sin(t * 1.9 + dot(at.xz, vec2(0.013, 0.017)) + length(p.xz) * 0.15) * length(p.xz) * 0.03, 0.0)",
                    4.f);
            }
        }
        forest_visual.set_mesh(oak);
        const uint32_t birch_index = forest_visual.add_mesh(birch);
        const uint32_t pine_index = forest_visual.add_mesh(pine);
        forest_visual.add_instances({per_mesh[0].data(), per_mesh[0].size()});
        forest_visual.add_instances({per_mesh[1].data(), per_mesh[1].size()}, birch_index);
        forest_visual.add_instances({per_mesh[2].data(), per_mesh[2].size()}, pine_index);
        auto forest_element = velk::create_element();
        forest_trs = velk::trait::transform::create_trs();
        forest_element.add_trait(forest_trs);
        forest_element.add_trait(forest_visual);
        scene.add(root, forest_element);

        if (grass_count > 0) {
            const long grass_columns = std::max(1L, static_cast<long>(std::sqrt(static_cast<double>(grass_count))));
            const float patch_size = kTerrainExtent / static_cast<float>(grass_columns);
            constexpr float kGrassLine = 1100.f;
            constexpr float kMaxGrassSlope = 0.5f;
            velk::vector<velk::InstanceTransform> patches;
            patches.reserve(static_cast<size_t>(grass_columns * grass_columns));
            for (long i = 0; i < grass_columns * grass_columns; ++i) {
                const float x = (static_cast<float>(i % grass_columns) + 0.5f + 0.5f * forest_jitter(i, 3.7f)) * patch_size - kTerrainExtent * 0.5f;
                const float z = (static_cast<float>(i / grass_columns) + 0.5f + 0.5f * forest_jitter(i, 5.3f)) * patch_size - kTerrainExtent * 0.5f;
                const float height = terrain.height_at(x, z);
                const float dx = (terrain.height_at(x + 15.f, z) - terrain.height_at(x - 15.f, z)) / 30.f;
                const float dz = (terrain.height_at(x, z + 15.f) - terrain.height_at(x, z - 15.f)) / 30.f;
                if (height > kGrassLine || std::sqrt(dx * dx + dz * dz) > kMaxGrassSlope) {
                    continue;
                }
                // Turned and sheared to lie along the slope. Up is -y.
                const float turn = (forest_jitter(i, 17.9f) + 0.5f) * 6.2831853f;
                const float c = std::cos(turn);
                const float s = std::sin(turn);
                patches.push_back(velk::InstanceTransform{{{c, 0.f, s, x},
                                                           {-(dx * c - dz * s), 1.f, -(dx * s + dz * c), kFloorY - height},
                                                           {-s, 0.f, c, z}}});
            }
            auto grass_visual = velk::trait::visual::create_instanced_mesh();
            grass_visual.set_mesh(velk::Mesh(build_grass_patch(*render_context, patch_size * 1.3f, foliage(velk::color{0.17f, 0.29f, 0.07f, 1.f}))));
            grass_visual.add_instances({patches.data(), patches.size()});
            auto grass_element = velk::create_element();
            grass_element.add_trait(grass_visual);
            scene.add(root, grass_element);
            VELK_LOG(I, "scale: %zu grass patches", patches.size());
        }
    } else if (forest && !nature) {
        auto trunk_material = velk::material::create_standard(velk::color{0.3f, 0.2f, 0.12f, 1.f}, 0.f, 0.8f);
        auto crown_material = velk::material::create_standard(velk::color{0.15f, 0.42f, 0.14f, 1.f}, 0.f, 0.7f);
        // 2 cones * 64 sides * 39 rings * 2 triangles.
        velk::Mesh tree_mesh(rocks ? build_rock_mesh(*render_context, 40.f,
                                                     velk::material::create_standard(velk::color{0.5f, 0.46f, 0.4f, 1.f}, 0.f, 0.8f))
                             : blocks ? build_block_mesh(*render_context, 24.f, 12.f,
                                                         velk::material::create_standard(velk::color{0.6f, 0.25f, 0.2f, 1.f}, 0.f, 0.6f))
                             : heavy ? build_tree_mesh(*render_context, kTreeHeight, trunk_material, crown_material, 64u, 39u)
                                     : build_tree_mesh(*render_context, kTreeHeight, trunk_material, crown_material));
        if (forest_instances) {
            velk::vector<velk::InstanceTransform> transforms;
            transforms.resize(static_cast<size_t>(element_count));
            for (long i = 0; i < element_count; ++i) {
                transforms[static_cast<size_t>(i)] = velk::InstanceTransform::from(velk::mat4::translate(tree_position(i)));
            }
            forest_visual = velk::trait::visual::create_instanced_mesh();
            forest_visual.set_mesh(tree_mesh);
            if (residency_mb != 0) {
                forest_visual.set_residency_budget(uint64_t(residency_mb) * 1000000u);
            }
            if (draw_near) {
                forest_visual.set_draw_distance(3000.f);
            }
            if (wind) {
                // The tallest tree reaches about 1.6 tree heights, which at the largest
                // angle bounds the displacement.
                forest_visual.set_motion(R"(
mat4 velk_instance_motion(mat4 rest, uint slot, vec4 params)
{
    vec3 p = rest[3].xyz;
    float gust = sin(params.x * 1.3 + p.x * 0.004 + p.z * 0.003);
    float angle = 0.06 + 0.06 * gust + 0.02 * sin(params.x * 3.1 + float(slot) * 1.7);
    float c = cos(angle);
    float s = sin(angle);
    return rest * mat4(c, s, 0.0, 0.0, -s, c, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0);
}
)", kTreeHeight * 1.7f * 0.2f);
            }
            if (species) {
                const velk::IMaterial::Ptr pale = velk::material::create_standard(velk::color{0.45f, 0.5f, 0.12f, 1.f}, 0.f, 0.7f);
                const velk::IMaterial::Ptr dark = velk::material::create_standard(velk::color{0.05f, 0.25f, 0.16f, 1.f}, 0.f, 0.7f);
                const uint32_t tall = forest_visual.add_mesh(build_tree_mesh(*render_context, kTreeHeight * 1.6f, trunk_material, dark));
                const uint32_t small = forest_visual.add_mesh(build_tree_mesh(*render_context, kTreeHeight * 0.6f, trunk_material, pale));
                velk::vector<velk::InstanceTransform> per_mesh[3];
                for (long i = 0; i < element_count; ++i) {
                    per_mesh[i % 3].push_back(transforms[static_cast<size_t>(i)]);
                }
                forest_visual.add_instances({per_mesh[0].data(), per_mesh[0].size()});
                forest_visual.add_instances({per_mesh[1].data(), per_mesh[1].size()}, tall);
                forest_visual.add_instances({per_mesh[2].data(), per_mesh[2].size()}, small);
            } else {
                forest_visual.add_instances({transforms.data(), transforms.size()});
            }
            auto forest_element = velk::create_element();
            forest_trs = velk::trait::transform::create_trs();
            forest_element.add_trait(forest_trs);
            forest_element.add_trait(forest_visual);
            scene.add(root, forest_element);

            if (real_trees) {
                forest_visual.set_far_form(false);
            }
        } else {
            for (long i = 0; i < element_count; ++i) {
                auto element = velk::create_element();
                auto trs = velk::trait::transform::create_trs();
                trs.set_translate(tree_position(i));
                auto visual = velk::trait::visual::create_mesh();
                visual.set_mesh(tree_mesh);
                element.add_trait(trs);
                element.add_trait(visual);
                scene.add(root, element);
            }
        }
    }

    for (long i = 0; i < element_count && !forest; ++i) {
        auto element = velk::create_element();
        auto trs = velk::trait::transform::create_trs();
        const velk::vec3 position{grid_start + static_cast<float>(i % columns) * kElementSpacing, -kElementSize,
                                  grid_start + static_cast<float>(i / columns) * kElementSpacing};
        trs.set_translate(position);

        // Movers are spread evenly through the grid.
        const bool is_mover = next_mover < mover_count && i == next_mover * element_count / mover_count;

        velk::Element parent = root;
        if (group_movers && is_mover) {
            parent = velk::create_element();
            parent.add_trait(trs);
            scene.add(root, parent);
        } else {
            element.add_trait(trs);
        }

        switch (deform_movers && is_mover ? 4 : i % 4) {
        case 4: {
            auto visual = velk::trait::visual::create_mesh();
            visual.set_mesh(velk::Mesh(render_context->get_mesh_builder().build_deforming(deform_source)));
            element.add_trait(visual);
            if (gpu_deform) {
                auto deformer = velk::trait::deform::create_compute_mesh();
                deformer.set_deform(R"(
VelkVertex3D velk_mesh_deform(VelkVertex3D rest, uint index, vec4 params)
{
    float wave = sin(rest.position.y * 0.25 + params.x * 4.0);
    VelkVertex3D v = rest;
    v.position = rest.position + rest.normal * (wave * 4.0);
    return v;
}
)", 4.01f);
                element.add_trait(deformer);
                gpu_ripples.push_back(deformer);
            } else {
                auto ripple = velk::instance().create<IRipple>(Ripple::static_class_id());
                element.add_trait(velk::Trait(interface_pointer_cast<velk::ITrait>(ripple)));
                ripples.push_back(ripple);
            }
            break;
        }
        case 0: {
            auto visual = velk::trait::visual::create_mesh();
            visual.set_mesh(triangle_mesh);
            element.add_trait(visual);
            break;
        }
        case 1: {
            auto visual = velk::ui::trait::visual::create_rect();
            if (recolor_rects) {
                visual.set_color({0.5f, 0.5f, 0.5f, 1.f});
                rects.push_back(visual);
            } else {
                visual.set_paint(rect_material);
            }
            element.add_trait(velk::ui::trait::layout::create_fixed_size(size_px, size_px));
            element.add_trait(visual);
            break;
        }
        case 2: {
            auto visual = velk::trait::visual::create_cube();
            visual.set_mesh(cube_mesh);
            element.add_trait(velk::ui::trait::layout::create_fixed_size(size_px, size_px, size_px));
            element.add_trait(visual);
            break;
        }
        default: {
            auto visual = velk::trait::visual::create_sphere();
            visual.set_mesh(sphere_mesh);
            element.add_trait(velk::ui::trait::layout::create_fixed_size(size_px, size_px, size_px));
            element.add_trait(visual);
            break;
        }
        }
        scene.add(parent, element);

        if (is_mover) {
            if (!deform_movers) {
                movers.push_back({trs, position, static_cast<float>(next_mover)});
            }
            ++next_mover;
        }
    }

    if (with_text) {
        const velk::vec3 position{-400.f, -300.f, 0.f};
        auto text = velk::create_element();
        auto trs = velk::trait::transform::create_trs();
        trs.set_translate(position);
        auto visual = velk::ui::trait::visual::create_text();
        visual.set_text("glyph records");
        visual.set_font_size(110.f);
        visual.set_color({0.95f, 0.55f, 1.f, 1.f});
        text.add_trait(velk::ui::trait::layout::create_fixed_size(velk::dim::px(800.f), velk::dim::px(180.f)));
        text.add_trait(trs);
        text.add_trait(visual);
        scene.add(root, text);
        if (mover_count > 0) {
            movers.push_back({trs, position, 0.f});
        }
    }

    app.add_view(window, camera);

    // Right drag orbits, the wheel zooms, and WASD moves over the ground. A flyby or a
    // held stop takes the camera back each frame.
    bool orbit_dragging = false;
    float orbit_last_x = 0.f;
    float orbit_last_y = 0.f;

    // In nature mode the camera sits at the focus and flies: W, A, S and D held, the wheel
    // for a jump along the view. Faster the higher it is above the ground.
    bool fly_keys[4] = {false, false, false, false};
    int gi_debug_view = 0;
    // Until then the camera follows a slow loop over the forest.
    bool touring = nature && hold_stop < 0 && !has_view && !no_tour;
    float tour_height = 0.f;
    const auto tour_point = [](float along) {
        constexpr float kTourRadius = 7000.f;
        const float angle = along / kTourRadius;
        const float radius = kTourRadius + 2200.f * std::sin(angle * 2.f + 0.7f);
        return velk::vec3{radius * std::cos(angle), 0.f, radius * std::sin(angle)};
    };
    const auto fly = [&](float forward_seconds, float right_seconds) {
        auto state = velk::read_state<velk::IOrbit>(static_cast<velk::IOrbit::Ptr>(orbit));
        auto focus_state = velk::read_state<velk::ITrs>(static_cast<velk::ITrs::Ptr>(focus_trs));
        if (!state || !focus_state) {
            return;
        }
        velk::vec3 at = focus_state->translate;
        const float speed = std::max(150.f, std::abs(ground_y(at.x, at.z) - at.y));
        const float yaw = state->yaw * 3.14159265f / 180.f;
        const float pitch = state->pitch * 3.14159265f / 180.f;
        const velk::vec3 forward{-std::sin(yaw) * std::cos(pitch), -std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
        const velk::vec3 right{std::cos(yaw), 0.f, -std::sin(yaw)};
        at = at + forward * (forward_seconds * speed) + right * (right_seconds * speed);
        focus_trs.set_translate(at);
    };

    velk::ScopedHandler pointer_sub = window.add_on_pointer_event([&](const velk::ui::PointerEvent& e) {
        if (e.action == velk::ui::PointerAction::Down && e.button == velk::ui::PointerButton::Right) {
            touring = false;
            orbit_dragging = true;
            orbit_last_x = e.position.x;
            orbit_last_y = e.position.y;
        } else if (e.action == velk::ui::PointerAction::Up && e.button == velk::ui::PointerButton::Right) {
            orbit_dragging = false;
        } else if (e.action == velk::ui::PointerAction::Move && orbit_dragging) {
            if (auto state = velk::read_state<velk::IOrbit>(static_cast<velk::IOrbit::Ptr>(orbit))) {
                // Flying, the view turns the way the mouse moves.
                const float turn = nature ? -0.2f : 0.3f;
                orbit.set_yaw(state->yaw + (e.position.x - orbit_last_x) * turn);
                const float pitch = state->pitch + (e.position.y - orbit_last_y) * turn;
                orbit.set_pitch(nature ? std::clamp(pitch, -89.f, 89.f) : pitch);
            }
            orbit_last_x = e.position.x;
            orbit_last_y = e.position.y;
        }
    });
    velk::ScopedHandler scroll_sub(window.input()->on_scroll_event(), [&](const velk::ui::ScrollEvent& e) {
        if (nature) {
            touring = false;
            fly(e.delta.y > 0 ? 0.5f : -0.5f, 0.f);
            return;
        }
        if (auto state = velk::read_state<velk::IOrbit>(static_cast<velk::IOrbit::Ptr>(orbit))) {
            orbit.set_distance(state->distance * (e.delta.y > 0 ? 0.9f : 1.1f));
        }
    });
    velk::ScopedHandler key_sub = window.add_on_key_event([&](const velk::ui::KeyEvent& e) {
        const bool held = e.action != velk::ui::KeyAction::Up;
        if (e.key == 87) fly_keys[0] = held;
        if (e.key == 83) fly_keys[1] = held;
        if (e.key == 65) fly_keys[2] = held;
        if (e.key == 68) fly_keys[3] = held;
        if (fly_keys[0] || fly_keys[1] || fly_keys[2] || fly_keys[3]) {
            touring = false;
        }
        if (!held) {
            return;
        }
        // Key code of G: steps through the GI debug views and back to the image.
        if (e.key == 71 && e.action == velk::ui::KeyAction::Down) {
            gi_debug_view = (gi_debug_view + 1) % 8;
            const char text[2] = {static_cast<char>('0' + gi_debug_view), 0};
#ifdef _WIN32
            _putenv_s("VELK_GI_DEBUG", text);
#else
            setenv("VELK_GI_DEBUG", text, 1);
#endif
            VELK_LOG(I, "scale: GI debug view %d", gi_debug_view);
            return;
        }
        auto state = velk::read_state<velk::IOrbit>(static_cast<velk::IOrbit::Ptr>(orbit));
        auto focus_state = velk::read_state<velk::ITrs>(static_cast<velk::ITrs::Ptr>(focus_trs));
        if (!state || !focus_state) {
            return;
        }
        // Key code of C.
        if (e.key == 67) {
            VELK_LOG(I, "scale: view=%.3f,%.3f,%.3f,%.3f,%.3f,%.3f", focus_state->translate.x, focus_state->translate.y,
                     focus_state->translate.z, state->yaw, state->pitch, state->distance);
            return;
        }
        if (nature) {
            return;
        }
        // Key codes of W, A, S and D.
        const float step = state->distance * 0.1f;
        velk::vec3 at = focus_state->translate;
        // Where the camera points, and to its right.
        const float yaw = state->yaw * 3.14159265f / 180.f;
        const float pitch = state->pitch * 3.14159265f / 180.f;
        const velk::vec3 forward{-std::sin(yaw) * std::cos(pitch), -std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
        const velk::vec3 right{std::cos(yaw), 0.f, -std::sin(yaw)};
        const float move = (e.key == 87 ? step : 0.f) - (e.key == 83 ? step : 0.f);
        const float strafe = (e.key == 68 ? step : 0.f) - (e.key == 65 ? step : 0.f);
        at = at + forward * move + right * strafe;
        focus_trs.set_translate(at);
    });

    if (with_overlay) {
        velk::PerformanceOverlayConfig overlay;
        overlay.enabled = true;
        app.set_performance_overlay(window, overlay);
    }

    using Clock = std::chrono::steady_clock;
    auto report_start = Clock::now();
    long frame = 0;
    long report_frame = 0;
    velk::vector<PassSamples> gpu_passes;
    velk::vector<velk::Element> extra_cubes;
    long next_extra = 0;

    const FlybyStop flyby_stops[] = {{"low", "climb", 200.f, -12.f},
                                     {"across", "rise", 7000.f, -12.f},
                                     {"top", "descend", std::max(extent, 7000.f), -89.f}};
    constexpr long kFlybyStops = 3;
    constexpr long kFlybySections = kFlybyStops * 2;
    velk::vector<PassSamples> flyby_passes[kFlybySections];
    auto last_frame_time = Clock::now();
    auto last_fly_time = Clock::now();

    while (app.poll()) {
        // Motion follows the frame number, so runs are comparable.
        const float t = static_cast<float>(frame) / 60.f;

        const auto fly_time = Clock::now();
        if (nature && hold_stop < 0) {
            const float seconds = std::chrono::duration<float>(fly_time - last_fly_time).count();
            const float forward = (fly_keys[0] ? seconds : 0.f) - (fly_keys[1] ? seconds : 0.f);
            const float right = (fly_keys[3] ? seconds : 0.f) - (fly_keys[2] ? seconds : 0.f);
            if (forward != 0.f || right != 0.f) {
                fly(forward, right);
            }
            if (touring) {
                // Fast and high, slowing every now and then to pass low over the ground.
                const float wave = 0.5f + 0.5f * std::sin(tour_along * 0.00045f + 1.f);
                const float calm = wave * wave * wave;
                const float tour_speed = 650.f + (70.f - 650.f) * calm;
                tour_along += seconds * tour_speed;
                const velk::vec3 at = tour_point(tour_along);
                // Clear of the highest ground in the next few seconds, rising and sinking smoothly.
                float clear_y = ground_y(at.x, at.z);
                for (int ahead = 1; ahead <= 12; ++ahead) {
                    const velk::vec3 next = tour_point(tour_along + static_cast<float>(ahead) * tour_speed * 0.4f);
                    clear_y = std::min(clear_y, ground_y(next.x, next.z));
                }
                const float altitude = 450.f + (55.f - 450.f) * calm;
                const float target_y = clear_y - altitude;
                tour_height = frame == 0 ? target_y : tour_height + (target_y - tour_height) * std::min(seconds * 0.6f, 1.f);
                focus_trs.set_translate({at.x, tour_height, at.z});

                // Along the path, turned somewhat inward so the terrain's edge stays behind.
                const velk::vec3 ahead_point = tour_point(tour_along + 500.f);
                const velk::vec3 along_path = velk::vec3::normalize({ahead_point.x - at.x, 0.f, ahead_point.z - at.z});
                const velk::vec3 inward = velk::vec3::normalize({-at.x, 0.f, -at.z});
                const velk::vec3 facing = velk::vec3::normalize(along_path * 0.7f + inward * 0.3f);
                orbit.set_yaw(std::atan2(-facing.x, -facing.z) * 180.f / 3.14159265f);
                orbit.set_pitch(-12.f + (-3.f + 12.f) * calm);
            }
        }
        last_fly_time = fly_time;

        if (hold_stop >= 0 && forest) {
            orbit.set_distance(hold_distance > 0.f ? hold_distance : flyby_stops[hold_stop].distance);
            orbit.set_pitch(hold_pitch != 0.f ? hold_pitch : flyby_stops[hold_stop].pitch);
            orbit.set_yaw(hold_yaw);
        }

        long flyby_section = 0;
        if (flyby && forest) {
            const long leg_frames = kFlybyHoldFrames + kFlybyMoveFrames;
            const long round_frame = frame % (leg_frames * kFlybyStops);
            const long leg = round_frame / leg_frames;
            const long within = round_frame - leg * leg_frames;
            const FlybyStop& from = flyby_stops[leg];
            const FlybyStop& to = flyby_stops[(leg + 1) % kFlybyStops];
            float blend = 0.f;
            flyby_section = leg * 2;
            if (within >= kFlybyHoldFrames) {
                const float f = static_cast<float>(within - kFlybyHoldFrames) / static_cast<float>(kFlybyMoveFrames);
                blend = f * f * (3.f - 2.f * f);
                flyby_section = leg * 2 + 1;
            }
            // Evenly in ratio, so a climb feels the same speed at all heights.
            orbit.set_distance(from.distance * std::pow(to.distance / from.distance, blend));
            orbit.set_pitch(from.pitch + (to.pitch - from.pitch) * blend);
            orbit.set_yaw(60.f);
            // Back and forth over a quarter of the forest, at up to 800 units a second.
            const float reach = extent * 0.25f;
            const float focus_x = reach * std::sin(t * 800.f / reach);
            focus_trs.set_translate({focus_x, ground_y(focus_x, 0.f) - 30.f, 0.f});
        }

        const bool movers_still = burst_movers && (frame / 200) % 2 == 1;
        for (auto& mover : movers) {
            if (movers_still) {
                break;
            }
            const float a = t + mover.phase;
            if (rotate_only) {
                mover.trs.set_rotation({a * 30.f, a * 45.f, 0.f});
            } else if (escape_movers) {
                mover.trs.set_translate({mover.base.x, mover.base.y - 3000.f * std::max(0.f, std::sin(t)),
                                         mover.base.z});
            } else {
                mover.trs.set_translate({mover.base.x + std::sin(a) * 30.f, mover.base.y + std::sin(a * 1.7f) * 30.f,
                                         mover.base.z + std::cos(a) * 30.f});
            }
        }

        if (forest && forest_visual && wind) {
            forest_visual.set_motion_params({t, 0.f, 0.f, 0.f});
        }
        if (forest_shift) {
            forest_trs.set_translate({std::sin(t) * 100.f, 0.f, 0.f});
        }
        if (forest_churn && element_count > 0) {
            // The slot just freed is reused, so this keeps the same slot in use.
            const auto removed = static_cast<uint32_t>(frame % element_count);
            forest_visual.remove_instance(removed);
            forest_visual.add_instance(velk::mat4::translate(tree_position(removed) + velk::vec3{0.f, 0.f, 20.f}));
            if (forest_extra_slot == 0xFFFFFFFFu) {
                forest_extra_slot = forest_visual.add_instance(velk::mat4::translate({grid_start, kFloorY, grid_start - kElementSpacing}));
            }
        }
        if (forest_instances && !species) {
            // Scattered over the forest, the same trees in every run.
            for (long m = 0; m < mover_count; ++m) {
                const auto i = static_cast<long>(static_cast<uint64_t>(m) * 2654435761ull % static_cast<uint64_t>(element_count));
                const float a = t * 2.f + static_cast<float>(m);
                const float size = !mixed ? 1.f : m % 100 == 0 ? 30.f : m % 10 == 0 ? 5.f : 0.5f;
                const velk::mat4 sized = velk::mat4::scale({size, size, size});
                if (forest_cluster) {
                    // About a tree's own size per tree, so they crowd without all overlapping.
                    const float side = std::cbrt(static_cast<float>(mover_count)) * 32.f;
                    velk::vec3 at{forest_jitter(m, 12.9898f) * side,
                                  kFloorY - side * 0.5f - 100.f + forest_jitter(m, 78.233f) * side,
                                  forest_jitter(m, 39.346f) * side};
                    if (outlier && m == 0) {
                        at.x += 2000000.f;
                    }
                    forest_visual.set_transform(static_cast<uint32_t>(i),
                                                velk::mat4::translate(at + velk::vec3{std::sin(a) * 3.f, 0.f, std::cos(a) * 3.f}) * sized);
                    continue;
                }
                forest_visual.set_transform(static_cast<uint32_t>(i),
                                            velk::mat4::translate(tree_position(i) + velk::vec3{std::sin(a) * 30.f, 0.f, std::cos(a) * 30.f}) * sized);
            }
        }

        for (auto& deformer : gpu_ripples) {
            deformer.set_params({t, 0.f, 0.f, 0.f});
        }
        for (auto& ripple : ripples) {
            ripple->time().set_value(t);
            if (scatter_movers) {
                ripple->scatter().set_value(std::min(t, 1.f) * kElementSize * 0.25f);
            }
        }

        if (recolor_rects && frame == 10) {
            for (auto& rect : rects) {
                rect.set_color({1.f, 0.05f, 0.05f, 1.f});
            }
        }

        if (churn_elements && frame % 20 == 0) {
            constexpr size_t kExtraCubes = 8;
            if (extra_cubes.size() == kExtraCubes) {
                scene.remove(extra_cubes.front());
                extra_cubes.erase(extra_cubes.begin());
            }
            auto cube = velk::create_element();
            auto trs = velk::trait::transform::create_trs();
            trs.set_translate({grid_start + static_cast<float>(next_extra % columns) * kElementSpacing,
                               -kElementSize * 2.5f, grid_start + kElementSpacing * 0.5f});
            auto visual = velk::trait::visual::create_cube();
            visual.set_mesh(cube_mesh);
            cube.add_trait(velk::ui::trait::layout::create_fixed_size(size_px, size_px, size_px));
            cube.add_trait(trs);
            cube.add_trait(visual);
            scene.add(root, cube);
            extra_cubes.push_back(cube);
            ++next_extra;
        }

        app.update();
        app.present();
        ++frame;


        const auto now = Clock::now();
        if (backend && frame > kWarmupFrames) {
            auto& passes = flyby && forest ? flyby_passes[flyby_section] : gpu_passes;
            record_gpu_timings(passes, backend->last_gpu_timings());
            if (flyby && forest) {
                velk::GpuPassTiming frame_time{};
                frame_time.label = "whole frame";
                frame_time.ms = static_cast<float>(std::chrono::duration<double, std::milli>(now - last_frame_time).count());
                record_gpu_timings(passes, {&frame_time, 1});
            }
        }
        last_frame_time = now;

        const double elapsed = std::chrono::duration<double>(now - report_start).count();
        if (elapsed >= 1.0) {
            const long frames = frame - report_frame;
            VELK_LOG(I, "scale: frame %ld, %.2f ms/frame", frame, elapsed * 1000.0 / frames);
            report_start = now;
            report_frame = frame;
        }

        if (frame_limit > 0 && frame >= frame_limit) {
            break;
        }
    }

    print_gpu_timings(gpu_passes);
    for (long section = 0; section < kFlybySections; ++section) {
        if (!flyby_passes[section].empty()) {
            const FlybyStop& stop = flyby_stops[section / 2];
            VELK_LOG(I, "scale: flyby section %s", section % 2 == 0 ? stop.name : stop.move_name);
            print_gpu_timings(flyby_passes[section]);
        }
    }
    return 0;
}
