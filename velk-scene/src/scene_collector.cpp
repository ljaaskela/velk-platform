#include "scene_collector.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace velk {

void emit_mesh_shapes(IVisual* visual, IMesh& mesh, const mat4& world, const color& tint, IProgram* paint,
                      ShapeCb cb, void* user)
{
    for (auto& prim_ptr : mesh.get_primitives()) {
        auto prim_state = read_state<IMeshPrimitive>(prim_ptr.get());
        if (!prim_state) continue;
        auto buf = prim_ptr->get_buffer();
        if (!buf) continue;
        // Skip until the geometry is GPU-resident. Asked as a ref
        // rather than an address: the bytes live in the mesh-word
        // arena, so there is a base and no address to test.
        if (!get_gpu_ref(buf).valid()) continue;
        uint32_t v_count = prim_ptr->get_vertex_count();
        uint32_t i_count = prim_ptr->get_index_count();
        uint32_t v_stride = prim_ptr->get_vertex_stride();
        if (v_count == 0 || i_count < 3 || v_stride == 0) continue;
        if (prim_ptr->get_topology() != MeshTopology::TriangleList) continue;

        // Per-primitive material (pulled the same way as
        // analytic CubeVisual / SphereVisual, but per
        // primitive instead of just first).
        IProgram* prim_paint = paint;
        if (prim_state->material) {
            auto prog_ptr = prim_state->material.template get<IProgram>();
            if (prog_ptr) prim_paint = prog_ptr.get();
        }

        // World-space AABB by transforming the local AABB's
        // 8 corners through the element world matrix.
        aabb local_b = prim_ptr->get_bounds();
        float lminx = local_b.position.x;
        float lminy = local_b.position.y;
        float lminz = local_b.position.z;
        float lmaxx = lminx + local_b.extent.width;
        float lmaxy = lminy + local_b.extent.height;
        float lmaxz = lminz + local_b.extent.depth;
        auto xf = [&](float lx, float ly, float lz) {
            float wx_ = world(0, 0) * lx + world(0, 1) * ly + world(0, 2) * lz + world(0, 3);
            float wy_ = world(1, 0) * lx + world(1, 1) * ly + world(1, 2) * lz + world(1, 3);
            float wz_ = world(2, 0) * lx + world(2, 1) * ly + world(2, 2) * lz + world(2, 3);
            return ::velk::vec3{wx_, wy_, wz_};
        };
        ::velk::vec3 c[8] = {
            xf(lminx, lminy, lminz), xf(lmaxx, lminy, lminz),
            xf(lminx, lmaxy, lminz), xf(lmaxx, lmaxy, lminz),
            xf(lminx, lminy, lmaxz), xf(lmaxx, lminy, lmaxz),
            xf(lminx, lmaxy, lmaxz), xf(lmaxx, lmaxy, lmaxz),
        };
        float wmin[3] = {c[0].x, c[0].y, c[0].z};
        float wmax[3] = {c[0].x, c[0].y, c[0].z};
        for (int k = 1; k < 8; ++k) {
            wmin[0] = std::min(wmin[0], c[k].x); wmax[0] = std::max(wmax[0], c[k].x);
            wmin[1] = std::min(wmin[1], c[k].y); wmax[1] = std::max(wmax[1], c[k].y);
            wmin[2] = std::min(wmin[2], c[k].z); wmax[2] = std::max(wmax[2], c[k].z);
        }

        ShapeSite site{};
        site.visual = visual;
        site.paint = prim_paint;
        site.world = &world;
        auto& s = site.geometry;
        s.origin[0] = lminx; s.origin[1] = lminy; s.origin[2] = lminz;
        s.u_axis[0] = lmaxx; s.u_axis[1] = lmaxy; s.u_axis[2] = lmaxz;
        s.color[0] = tint.r; s.color[1] = tint.g; s.color[2] = tint.b; s.color[3] = tint.a;
        s.shape_kind = kRtShapeKindMesh;
        s.mesh_static_base = kInvalidMeshStaticBase;  // resolved by the renderer cb.
        site.aabb_min[0] = wmin[0]; site.aabb_min[1] = wmin[1]; site.aabb_min[2] = wmin[2];
        site.aabb_max[0] = wmax[0]; site.aabb_max[1] = wmax[1]; site.aabb_max[2] = wmax[2];
        site.mesh_primitive = prim_ptr.get();
        site.has_mesh_data = true;

        cb(user, site);
    }
}

void emit_shapes_for_element(IElement* element, IRenderContext* ctx,
                             ShapeCb cb, void* user)
{
    auto* storage = interface_cast<IObjectStorage>(element);
    if (!storage) {
        return;
    }
    auto es = read_state<IElement>(element);
    if (!es) return;

    const ::velk::mat4& world = es->world_matrix;
    const float ew = es->size.width;
    const float eh = es->size.height;
    const float ed = es->size.depth;

    // Element-level world AABB: works as a sane default for shape
    // kinds whose `geometry` fields are basis vectors, not AABB
    // min/max (i.e. analytics, 2D rects). Mesh shapes overwrite this
    // with their own tight per-primitive world AABB below.
    aabb elem_aabb = es->world_aabb;
    const ::velk::vec3 elem_min = elem_aabb.min();
    const ::velk::vec3 elem_max = elem_aabb.max();

    for (size_t j = 0; j < storage->attachment_count(); ++j) {
        auto* visual = interface_cast<IVisual>(storage->get_attachment(j));
        if (!visual) continue;


        IProgram* paint = nullptr;
        ::velk::color tint = ::velk::color::white();
        if (auto vs2d = read_state<IVisual2D>(visual)) {
            // 2D-authoring affordances (color + paint) live on IVisual2D.
            // 3D visuals have neither; color defaults to white and the
            // material lives on the mesh's first primitive.
            tint = vs2d->color;
            if (vs2d->paint) {
                auto prog_ptr = vs2d->paint.template get<IProgram>();
                paint = prog_ptr.get();
            }
        } else if (auto vs3d = read_state<IVisual3D>(visual)) {
            // 3D visual fallback: reach through IVisual3D::mesh to the
            // first primitive's material. Matches what CubeVisual /
            // SphereVisual stamp onto DrawEntry::material for the raster
            // path; here we feed the same program into the RT shape's
            // material_id resolution so cubes/spheres with authored
            // materials shade correctly in primary and bounce rays.
            auto mesh_obj = vs3d->mesh.template get<IMesh>();
            if (mesh_obj) {
                auto prims = mesh_obj->get_primitives();
                if (prims.size() > 0) {
                    if (auto ps = read_state<IMeshPrimitive>(prims[0].get()); ps && ps->material) {
                        auto prog_ptr = ps->material.template get<IProgram>();
                        paint = prog_ptr.get();
                    }
                }
            }
        }

        auto* analytic = interface_cast<IAnalyticShape>(visual);
        uint32_t shape_kind = analytic ? analytic->get_shape_kind() : 0;

        if (shape_kind != 0) {
            ShapeSite site{};
            site.visual = visual;
            site.paint = paint;
            site.world = &world;
            auto& s = site.geometry;
            s.u_axis[0] = ew;
            s.v_axis[1] = eh;
            s.w_axis[2] = ed;
            s.color[0] = tint.r;
            s.color[1] = tint.g;
            s.color[2] = tint.b;
            s.color[3] = tint.a;
            if (shape_kind == 2) {
                s.params[0] = std::min({ew, eh, ed}) * 0.5f;
            }
            s.shape_kind = shape_kind;
            site.aabb_min[0] = elem_min.x; site.aabb_min[1] = elem_min.y; site.aabb_min[2] = elem_min.z;
            site.aabb_max[0] = elem_max.x; site.aabb_max[1] = elem_max.y; site.aabb_max[2] = elem_max.z;
            cb(user, site);
            continue;
        }

        // 3D visuals that aren't analytic primitives are triangle meshes (e.g.
        // MeshVisual from a glTF import). An instanced visual's shapes are emitted
        // by the BVH, once for all its instances.
        if (auto vs3d = read_state<IVisual3D>(visual)) {
            auto mesh_obj = vs3d->mesh.template get<IMesh>();
            if (mesh_obj && !interface_cast<IInstancedMeshVisual>(visual)) {
                emit_mesh_shapes(visual, *mesh_obj, world, tint, paint, cb, user);
            }
            continue;
        }

        float radius = 0.f;
        if (analytic) {
            auto* analytic_src = interface_cast<IShaderSource>(analytic);
            if (analytic_src && !analytic_src->get_source(shader_role::kIntersect).empty()) {
                radius = std::min(std::min(ew, eh) * 0.5f, 12.f);
            }
        }

        if (!ctx) continue;
        ::velk::size local_size{ew, eh, ed};
        auto entries = visual->get_draw_entries(*ctx, local_size);
        for (auto& dentry : entries) {
            // Instance layout — ElementInstance (see instance_types.h):
            //   [  0.. 63] mat4 world_matrix (raster only; zero here)
            //   [ 64.. 79] vec4 offset  (xy = local offset; z pad)
            //   [ 80.. 95] vec4 size    (xy = local extents; z pad)
            //   [ 96..111] vec4 color
            //   [112..115] uint shape_param (glyph index etc.)
            constexpr size_t kOffsetOff = 64;
            constexpr size_t kSizeOff   = 80;
            constexpr size_t kColorOff  = 96;
            constexpr size_t kParamsOff = 112;
            if (dentry.instance_size < kSizeOff + 8) continue;

            float local_px = 0, local_py = 0, sz_w = 0, sz_h = 0;
            std::memcpy(&local_px, dentry.instance_data + kOffsetOff,     4);
            std::memcpy(&local_py, dentry.instance_data + kOffsetOff + 4, 4);
            std::memcpy(&sz_w,     dentry.instance_data + kSizeOff,       4);
            std::memcpy(&sz_h,     dentry.instance_data + kSizeOff + 4,   4);

            float cr = tint.r, cg = tint.g, cb_ = tint.b, ca = tint.a;
            if (dentry.instance_size >= kColorOff + 16) {
                std::memcpy(&cr,  dentry.instance_data + kColorOff,      4);
                std::memcpy(&cg,  dentry.instance_data + kColorOff + 4,  4);
                std::memcpy(&cb_, dentry.instance_data + kColorOff + 8,  4);
                std::memcpy(&ca,  dentry.instance_data + kColorOff + 12, 4);
            }

            uint32_t shape_param = 0;
            if (dentry.instance_size >= kParamsOff + 4) {
                std::memcpy(&shape_param, dentry.instance_data + kParamsOff, 4);
            }

            ShapeSite site{};
            site.visual = visual;
            site.paint = paint;
            site.draw_entry = &dentry;
            site.world = &world;
            site.record_group = visual;
            auto& s = site.geometry;
            s.origin[0] = local_px;
            s.origin[1] = local_py;
            s.u_axis[0] = sz_w;
            s.v_axis[1] = sz_h;
            s.color[0] = cr; s.color[1] = cg; s.color[2] = cb_; s.color[3] = ca;
            s.params[0] = radius;
            s.shape_param = shape_param;
            site.aabb_min[0] = elem_min.x; site.aabb_min[1] = elem_min.y; site.aabb_min[2] = elem_min.z;
            site.aabb_max[0] = elem_max.x; site.aabb_max[1] = elem_max.y; site.aabb_max[2] = elem_max.z;
            cb(user, site);
        }
    }
}

void enumerate_scene_shapes(const SceneState& scene_state, IRenderContext* ctx,
                            ShapeCb cb, void* user)
{
    if (!scene_state.scene) return;
    walk_scene_preorder(scene_state.scene, scene_state.scene->root(),
        [&](IElement* elem) { emit_shapes_for_element(elem, ctx, cb, user); });
}

void build_local_tree(vector<RtShape>& shapes, vector<BlasNode>& tree)
{
    const auto count = static_cast<uint32_t>(shapes.size());
    vector<detail::BlasTriInfo> infos;
    infos.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        const RtShape& shape = shapes[i];
        auto& info = infos[i];
        info.index = i;
        for (int axis = 0; axis < 3; ++axis) {
            const float a = shape.origin[axis];
            const float b = a + shape.u_axis[axis] + shape.v_axis[axis];
            info.aabb_min[axis] = std::min(a, b);
            info.aabb_max[axis] = std::max(a, b);
            info.center[axis] = (a + b) * 0.5f;
        }
        // A flat box has no thickness for a slab test to enter.
        const float extent = std::max(info.aabb_max[0] - info.aabb_min[0],
                                      info.aabb_max[1] - info.aabb_min[1]);
        for (int axis = 0; axis < 3; ++axis) {
            info.aabb_min[axis] -= extent * 1e-3f;
            info.aabb_max[axis] += extent * 1e-3f;
        }
    }

    BlasBuild build;
    build.nodes.push_back({});
    detail::blas_fill_node(build, 0, infos, 0, infos.size());

    const vector<RtShape> original = shapes;
    shapes.clear();
    for (uint32_t index : build.triangle_indices) {
        shapes.push_back(original[index]);
    }
    tree = std::move(build.nodes);
}

BvhTree build_bvh_tree(array_view<const BvhItemBounds> items, uint32_t leaf_size, bool force_split)
{
    BvhTree out;
    if (items.empty()) {
        return out;
    }

    const auto item_count = static_cast<uint32_t>(items.size());
    struct ShapeRef
    {
        uint32_t index;        // index into the input arrays
        float    centroid[3];
    };
    vector<ShapeRef> refs;
    refs.resize(item_count);
    for (uint32_t i = 0; i < item_count; ++i) {
        refs[i].index = i;
        for (int k = 0; k < 3; ++k) {
            refs[i].centroid[k] = 0.5f * (items[i].min[k] + items[i].max[k]);
        }
    }

    constexpr float kInf = 1e30f;

    // Final item order = leaf-traversal order.
    out.order.reserve(item_count);

    struct BuildItem
    {
        uint32_t self_index;
        size_t   begin;
        size_t   end;
    };
    out.nodes.push_back({});
    vector<BuildItem> work;
    work.push_back({0u, 0u, refs.size()});

    while (!work.empty()) {
        BuildItem item = work.back();
        work.pop_back();

        // Compute this node's AABB (union of contained shape AABBs).
        float bmin[3] = { kInf,  kInf,  kInf};
        float bmax[3] = {-kInf, -kInf, -kInf};
        for (size_t i = item.begin; i < item.end; ++i) {
            const uint32_t idx = refs[i].index;
            for (int k = 0; k < 3; ++k) {
                if (items[idx].min[k] < bmin[k]) bmin[k] = items[idx].min[k];
                if (items[idx].max[k] > bmax[k]) bmax[k] = items[idx].max[k];
            }
        }
        GpuBvhNode& node = out.nodes[item.self_index];
        for (int k = 0; k < 3; ++k) {
            node.aabb_min[k] = bmin[k];
            node.aabb_max[k] = bmax[k];
        }
        node.aabb_min[3] = 0.f;
        node.aabb_max[3] = 0.f;

        const size_t count = item.end - item.begin;

        auto emit_leaf = [&]() {
            const uint32_t first = static_cast<uint32_t>(out.order.size());
            for (size_t i = item.begin; i < item.end; ++i) {
                out.order.push_back(refs[i].index);
            }
            node.first_shape = first;
            node.shape_count = static_cast<uint32_t>(count);
            node.first_child = 0;
            node.child_count = 0;
        };

        if (count <= leaf_size) {
            emit_leaf();
            continue;
        }

        // Pick split axis = longest centroid extent.
        float cmin[3] = { kInf,  kInf,  kInf};
        float cmax[3] = {-kInf, -kInf, -kInf};
        for (size_t i = item.begin; i < item.end; ++i) {
            for (int k = 0; k < 3; ++k) {
                if (refs[i].centroid[k] < cmin[k]) cmin[k] = refs[i].centroid[k];
                if (refs[i].centroid[k] > cmax[k]) cmax[k] = refs[i].centroid[k];
            }
        }
        int axis = 0;
        const float dx = cmax[0] - cmin[0];
        const float dy = cmax[1] - cmin[1];
        const float dz = cmax[2] - cmin[2];
        if (dy >= dx && dy >= dz)      axis = 1;
        else if (dz >= dx && dz >= dy) axis = 2;

        const size_t median = item.begin + count / 2;
        if (cmax[axis] == cmin[axis] && !force_split) {
            // All centroids coincide on the chosen axis, so no split can separate
            // them. The leaf is slower to test but still correct.
            emit_leaf();
            continue;
        }

        std::sort(refs.begin() + item.begin, refs.begin() + item.end,
                  [axis](const ShapeRef& a, const ShapeRef& b) {
                      return a.centroid[axis] < b.centroid[axis];
                  });

        // SAH split: among the `count - 1` candidate split positions
        // along the sorted axis, pick the one minimising the surface
        // area heuristic
        //   cost(k) = c_t + c_i * (sa(L)*|L| + sa(R)*|R|) / sa(parent)
        // and compare against the leaf cost `c_i * count`. If the best split
        // costs more than a leaf, it stays a leaf regardless of @p leaf_size,
        // unless @p force_split asks for a median split.
        constexpr float kCostTraversal = 1.0f;
        constexpr float kCostIntersect = 1.5f;
        auto surface_area = [](const float lo[3], const float hi[3]) {
            float sx = std::max(0.f, hi[0] - lo[0]);
            float sy = std::max(0.f, hi[1] - lo[1]);
            float sz = std::max(0.f, hi[2] - lo[2]);
            return 2.f * (sx * sy + sy * sz + sz * sx);
        };

        // Right-to-left sweep: right_lo[i] / right_hi[i] is the AABB
        // of refs[item.begin + i .. item.end). Sized `count`, the
        // last entry covers exactly the last shape.
        vector<float> right_lo(count * 3);
        vector<float> right_hi(count * 3);
        {
            float lo[3] = { kInf, kInf, kInf};
            float hi[3] = {-kInf,-kInf,-kInf};
            for (size_t i = count; i-- > 0;) {
                const uint32_t idx = refs[item.begin + i].index;
                for (int k = 0; k < 3; ++k) {
                    lo[k] = std::min(lo[k], items[idx].min[k]);
                    hi[k] = std::max(hi[k], items[idx].max[k]);
                    right_lo[i * 3 + k] = lo[k];
                    right_hi[i * 3 + k] = hi[k];
                }
            }
        }

        const float parent_sa = surface_area(bmin, bmax);
        const float leaf_cost = kCostIntersect * static_cast<float>(count);

        float best_cost = leaf_cost;
        size_t best_split = 0; // 0 == "no split, emit leaf"

        float left_lo[3] = { kInf, kInf, kInf};
        float left_hi[3] = {-kInf,-kInf,-kInf};
        for (size_t j = 0; j + 1 < count; ++j) {
            const uint32_t idx = refs[item.begin + j].index;
            for (int k = 0; k < 3; ++k) {
                left_lo[k] = std::min(left_lo[k], items[idx].min[k]);
                left_hi[k] = std::max(left_hi[k], items[idx].max[k]);
            }
            const float left_sa = surface_area(left_lo, left_hi);
            const float right_sa = surface_area(
                right_lo.data() + (j + 1) * 3, right_hi.data() + (j + 1) * 3);
            const float l_count = static_cast<float>(j + 1);
            const float r_count = static_cast<float>(count - j - 1);
            const float cost = kCostTraversal + kCostIntersect *
                (left_sa * l_count + right_sa * r_count) / std::max(parent_sa, 1e-30f);
            if (cost < best_cost) {
                best_cost = cost;
                best_split = j + 1;
            }
        }

        if (best_split == 0 && !force_split) {
            // Splitting is no better than a flat leaf at this node.
            emit_leaf();
            continue;
        }

        const size_t mid = best_split != 0 ? item.begin + best_split : median;

        const uint32_t left_idx  = static_cast<uint32_t>(out.nodes.size());
        out.nodes.push_back({});
        const uint32_t right_idx = static_cast<uint32_t>(out.nodes.size());
        out.nodes.push_back({});

        // Re-acquire the node reference: the push_backs above may have
        // reallocated `out.nodes`, dangling the `node` reference taken
        // earlier (line `out.nodes[item.self_index]`). The AABB writes
        // earlier in this iteration landed in the pre-reallocation
        // buffer and were copied to the new buffer, so they're fine.
        // The first_child / child_count writes below MUST go through
        // a fresh reference; otherwise they hit freed memory and the
        // actual node stays with zero-initialised children — making
        // it look like an empty leaf and dropping every shape under
        // that subtree from the BVH walk.
        GpuBvhNode& self = out.nodes[item.self_index];
        self.first_shape = 0;
        self.shape_count = 0;
        self.first_child = left_idx;
        self.child_count = 2;

        // Push right first so left is processed first (cheaper for the
        // refs array's cache locality on the next iteration).
        work.push_back({right_idx, mid,        item.end});
        work.push_back({left_idx,  item.begin, mid});
    }

    return out;
}

void enumerate_scene_lights(const SceneState& scene_state, LightCb cb, void* user)
{
    constexpr float kDegToRad = 0.017453292519943295f;

    if (!scene_state.scene) return;
    walk_scene_preorder(scene_state.scene, scene_state.scene->root(),
        [&](IElement* elem) {
            auto es = read_state<IElement>(elem);
            if (!es) return;
            auto* storage = interface_cast<IObjectStorage>(elem);
            if (!storage) return;

            for (size_t j = 0; j < storage->attachment_count(); ++j) {
                auto* light = interface_cast<ILight>(storage->get_attachment(j));
                if (!light) continue;
                auto ls = read_state<ILight>(light);
                if (!ls) continue;

                LightSite site{};
                site.element = elem;
                site.light = light;
                auto& g = site.base;
                g.flags[0] = static_cast<uint32_t>(ls->type);
                g.position[0] = es->world_matrix(0, 3);
                g.position[1] = es->world_matrix(1, 3);
                g.position[2] = es->world_matrix(2, 3);
                float fx = -es->world_matrix(0, 2);
                float fy = -es->world_matrix(1, 2);
                float fz = -es->world_matrix(2, 2);
                float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
                if (flen > 1e-6f) { fx /= flen; fy /= flen; fz /= flen; }
                g.direction[0] = fx;
                g.direction[1] = fy;
                g.direction[2] = fz;
                g.color_intensity[0] = ls->color.r;
                g.color_intensity[1] = ls->color.g;
                g.color_intensity[2] = ls->color.b;
                g.color_intensity[3] = ls->intensity;
                g.params[0] = ls->range;
                g.params[1] = std::cos(ls->cone_inner_deg * kDegToRad);
                g.params[2] = std::cos(ls->cone_outer_deg * kDegToRad);
                // params.w = light size for soft-shadow penumbra. Directional
                // size is authored as an angular diameter (deg) and stored as
                // the angular radius (rad); point/spot size is a world radius.
                // 0 stays 0 -> point source -> hard shadow (today's behavior).
                g.params[3] = (ls->type == LightType::Directional)
                                  ? ls->size * 0.5f * kDegToRad
                                  : ls->size;
                cb(user, site);
            }
        });
}

} // namespace velk
