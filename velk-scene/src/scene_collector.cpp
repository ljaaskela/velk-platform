#include "scene_collector.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace velk {

namespace {

// Pre-order DFS walk of the scene tree. `on_element` fires for each
// IElement encountered, before descending into its children. Children
// are visited in their natural storage order — callers that care about
// draw z-order should sort at each level themselves.
//
// Kept as a local template for readability; it's only instantiated
// inside this translation unit, so it doesn't bleed back into headers.
template <typename F>
void walk_scene_preorder(IScene* scene, const IObject::Ptr& obj, F&& on_element)
{
    if (auto* elem = interface_cast<IElement>(obj)) {
        on_element(elem);
        if (scene) {
            for (auto& kid : scene->children_of(obj)) {
                walk_scene_preorder(scene, kid, std::forward<F>(on_element));
            }
        }
    }
}

} // namespace

void emit_shapes_for_element(IElement* element, IRenderContext* ctx,
                             ShapeCb cb, void* user)
{
    auto* storage = interface_cast<IObjectStorage>(element);
    if (!storage) {
        return;
    }
    auto es = read_state<IElement>(element);
    if (!es) return;

    const float ox = es->world_matrix(0, 3);
    const float oy = es->world_matrix(1, 3);
    const float oz = es->world_matrix(2, 3);
    const float ux = es->world_matrix(0, 0);
    const float uy = es->world_matrix(1, 0);
    const float uz = es->world_matrix(2, 0);
    const float vx = es->world_matrix(0, 1);
    const float vy = es->world_matrix(1, 1);
    const float vz = es->world_matrix(2, 1);
    const float wx = es->world_matrix(0, 2);
    const float wy = es->world_matrix(1, 2);
    const float wz = es->world_matrix(2, 2);
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
            auto& s = site.geometry;
            s.origin[0] = ox; s.origin[1] = oy; s.origin[2] = oz;
            s.u_axis[0] = ux * ew; s.u_axis[1] = uy * ew; s.u_axis[2] = uz * ew;
            s.v_axis[0] = vx * eh; s.v_axis[1] = vy * eh; s.v_axis[2] = vz * eh;
            s.w_axis[0] = wx * ed; s.w_axis[1] = wy * ed; s.w_axis[2] = wz * ed;
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

        // 3D visuals that aren't analytic primitives are triangle
        // meshes (e.g. MeshVisual from a glTF import). Emit one
        // Mesh-kind RtShape per IMeshPrimitive, with a MeshData payload
        // pointing at the existing IMeshBuffer's VBO+IBO regions. The
        // renderer callback resolves mesh_data_addr to the GPU address of
        // its uploaded MeshData record.
        if (auto vs3d = read_state<IVisual3D>(visual)) {
            auto mesh_obj = vs3d->mesh.template get<IMesh>();
            if (mesh_obj) {
                const ::velk::mat4& world = es->world_matrix;
                ::velk::mat4 inv_world = ::velk::mat4::inverse(world);
                auto prims = mesh_obj->get_primitives();
                for (auto& prim_ptr : prims) {
                    auto prim_state = read_state<IMeshPrimitive>(prim_ptr.get());
                    if (!prim_state) continue;
                    auto buf = prim_ptr->get_buffer();
                    if (!buf) continue;
                    uint64_t buffer_addr = buf->get_gpu_address();
                    if (buffer_addr == 0) continue;
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
                    auto& s = site.geometry;
                    s.origin[0] = wmin[0]; s.origin[1] = wmin[1]; s.origin[2] = wmin[2];
                    s.u_axis[0] = wmax[0]; s.u_axis[1] = wmax[1]; s.u_axis[2] = wmax[2];
                    s.color[0] = tint.r; s.color[1] = tint.g; s.color[2] = tint.b; s.color[3] = tint.a;
                    s.shape_kind = kRtShapeKindMesh;
                    site.aabb_min[0] = wmin[0]; site.aabb_min[1] = wmin[1]; site.aabb_min[2] = wmin[2];
                    site.aabb_max[0] = wmax[0]; site.aabb_max[1] = wmax[1]; site.aabb_max[2] = wmax[2];

                    // Per-frame instance data: world matrices + a
                    // pointer placeholder. The renderer callback fills
                    // mesh_static_addr from the primitive's persistent
                    // IDrawData buffer (stable across frames). Static
                    // mesh metadata (buffer_addr, offsets, counts,
                    // stride) lives in that buffer — not duplicated
                    // here.
                    auto& mi = site.mesh_instance;
                    std::memcpy(mi.world,     world.m,     sizeof(mi.world));
                    std::memcpy(mi.inv_world, inv_world.m, sizeof(mi.inv_world));
                    mi.mesh_static_addr = 0;  // resolved by the renderer cb.
                    site.mesh_primitive = prim_ptr.get();
                    site.has_mesh_data = true;
                    (void)buffer_addr;  // validated above; no longer carried inline.

                    cb(user, site);
                }
            }
            continue;
        }

        float radius = 0.f;
        if (analytic && !analytic->get_shape_intersect_source().empty()) {
            radius = std::min(std::min(ew, eh) * 0.5f, 12.f);
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
            auto& s = site.geometry;
            s.origin[0] = ox + ux * local_px + vx * local_py;
            s.origin[1] = oy + uy * local_px + vy * local_py;
            s.origin[2] = oz + uz * local_px + vz * local_py;
            s.u_axis[0] = ux * sz_w; s.u_axis[1] = uy * sz_w; s.u_axis[2] = uz * sz_w;
            s.v_axis[0] = vx * sz_h; s.v_axis[1] = vy * sz_h; s.v_axis[2] = vz * sz_h;
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

BvhBuild build_scene_bvh(IScene* scene, IRenderContext* ctx,
                         ShapeCb cb, void* user)
{
    BvhBuild out;
    if (!scene) return out;

    auto root_obj = scene->root();
    auto root_elem = interface_pointer_cast<IElement>(root_obj);
    if (!root_elem) return out;

    // Step 1: collect all shapes (and their world AABBs + mesh
    // instance data) into flat arrays via a scene tree walk. The walk
    // order itself doesn't matter; we re-partition spatially below.
    vector<RtShape>          shapes;
    vector<MeshInstanceData> mesh_instances;
    vector<float>            shape_aabb_min; // 3 floats per shape
    vector<float>            shape_aabb_max;

    // Wrap the user cb in a local closure that also stuffs the shape
    // AABB + instance data into our flat arrays. The local closure is
    // adapted into a ShapeCb thunk for the inner emit call.
    struct CollectState {
        ShapeCb           user_cb;
        void*             user_user;
        vector<RtShape>*          shapes;
        vector<MeshInstanceData>* mesh_instances;
        vector<float>*            aabb_min;
        vector<float>*            aabb_max;
    };
    CollectState cs{cb, user, &shapes, &mesh_instances, &shape_aabb_min, &shape_aabb_max};
    auto collect_thunk = [](void* u, ShapeSite& site) {
        auto& s = *static_cast<CollectState*>(u);
        s.user_cb(s.user_user, site);
        s.shapes->push_back(site.geometry);
        s.mesh_instances->push_back(
            site.has_mesh_data ? site.mesh_instance : MeshInstanceData{});
        s.aabb_min->push_back(site.aabb_min[0]);
        s.aabb_min->push_back(site.aabb_min[1]);
        s.aabb_min->push_back(site.aabb_min[2]);
        s.aabb_max->push_back(site.aabb_max[0]);
        s.aabb_max->push_back(site.aabb_max[1]);
        s.aabb_max->push_back(site.aabb_max[2]);
    };
    walk_scene_preorder(scene, scene->root(), [&](IElement* elem) {
        emit_shapes_for_element(elem, ctx, collect_thunk, &cs);
    });

    if (shapes.empty()) {
        // Empty BVH: emit a single inverted-AABB root so trace_any_hit
        // sees node_count > 0 but every ray rejects.
        out.nodes.push_back({});
        out.root_index = 0;
        out.nodes[0].aabb_min[0] = 1.f; out.nodes[0].aabb_max[0] = -1.f;
        return out;
    }

    // Step 2: build a spatial median-split binary BVH over the flat
    // shape list. The previous scene-graph BVH had bistro-style scenes
    // putting thousands of shapes as direct children of one root node;
    // any iterative BVH walker with a fixed traversal stack (32 / 128)
    // silently dropped most of them, making large swaths of geometry
    // invisible to RT shadows. A binary tree with leaf threshold 4
    // bounds depth at ceil(log2(N/4)) — well under 32 for any
    // realistic shape count.
    const uint32_t shape_count = static_cast<uint32_t>(shapes.size());
    struct ShapeRef
    {
        uint32_t index;        // index into the input arrays
        float    centroid[3];
    };
    vector<ShapeRef> refs;
    refs.resize(shape_count);
    for (uint32_t i = 0; i < shape_count; ++i) {
        refs[i].index = i;
        for (int k = 0; k < 3; ++k) {
            refs[i].centroid[k] =
                0.5f * (shape_aabb_min[i * 3 + k] + shape_aabb_max[i * 3 + k]);
        }
    }

    constexpr uint32_t kLeafThreshold = 4;
    constexpr float    kInf = 1e30f;

    // Reorder buffers (final shape order = leaf-traversal order).
    vector<RtShape>          out_shapes;
    vector<MeshInstanceData> out_mesh_instances;
    out_shapes.reserve(shape_count);
    out_mesh_instances.reserve(shape_count);

    struct BuildItem
    {
        uint32_t self_index;
        size_t   begin;
        size_t   end;
    };
    out.nodes.push_back({});
    out.root_index = 0;
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
                if (shape_aabb_min[idx * 3 + k] < bmin[k]) bmin[k] = shape_aabb_min[idx * 3 + k];
                if (shape_aabb_max[idx * 3 + k] > bmax[k]) bmax[k] = shape_aabb_max[idx * 3 + k];
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
            const uint32_t first = static_cast<uint32_t>(out_shapes.size());
            for (size_t i = item.begin; i < item.end; ++i) {
                const uint32_t idx = refs[i].index;
                out_shapes.push_back(shapes[idx]);
                out_mesh_instances.push_back(mesh_instances[idx]);
            }
            node.first_shape = first;
            node.shape_count = static_cast<uint32_t>(count);
            node.first_child = 0;
            node.child_count = 0;
        };

        if (count <= kLeafThreshold) {
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

        if (cmax[axis] == cmin[axis]) {
            // All centroids coincide on the chosen axis: any sort
            // would oscillate forever. Fall back to a leaf even if
            // count > leaf threshold; the leaf is just slower to test
            // but still correct.
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
        // and compare against the leaf cost `c_i * count`. If the
        // best split is more expensive than a leaf we fall back to a
        // leaf — even if `count > kLeafThreshold` — since SAH says
        // the descent overhead exceeds the per-shape intersection cost.
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
                    lo[k] = std::min(lo[k], shape_aabb_min[idx * 3 + k]);
                    hi[k] = std::max(hi[k], shape_aabb_max[idx * 3 + k]);
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
                left_lo[k] = std::min(left_lo[k], shape_aabb_min[idx * 3 + k]);
                left_hi[k] = std::max(left_hi[k], shape_aabb_max[idx * 3 + k]);
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

        if (best_split == 0) {
            // Splitting is no better than a flat leaf at this node.
            emit_leaf();
            continue;
        }

        const size_t mid = item.begin + best_split;

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

    out.shapes         = std::move(out_shapes);
    out.mesh_instances = std::move(out_mesh_instances);
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
                cb(user, site);
            }
        });
}

} // namespace velk
