#include "scene_bvh.h"

#include <velk/api/perf.h>
#include <velk/api/state.h>
#include <velk/interface/intf_hierarchy.h>
#include <velk/interface/intf_object_storage.h>

#include <velk-scene/interface/intf_element.h>
#include <velk-scene/interface/intf_visual.h>

#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/plugin.h>

#include <cstring>

namespace velk::impl {

namespace {

constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnvPrime = 0x100000001b3ULL;

inline void hash_mix(uint64_t& h, uint64_t v)
{
    h = (h ^ v) * kFnvPrime;
}

/// Uploads @p size bytes into a FRESH region of @p arena and hands the old
/// @p region back to the arena (freed deferred past the in-flight frame's
/// fence). Allocating instead of overwriting in place is what makes a
/// mid-flight BVH change safe: a frame still reading the previous build never
/// sees a partially rewritten one. Returns the element base
/// (offset / @p stride), or 0 when there is nothing to upload.
uint32_t upload_region(IGpuArena* arena, ArenaRegion& region, const void* data,
                       uint64_t size, uint64_t stride, FrameContext& ctx)
{
    if (!arena || size == 0) {
        region = {};
        return 0;
    }
    auto fresh = arena->alloc(size, ctx);
    if (!fresh.valid()) {
        region = {};
        return 0;
    }
    const uint64_t offset = fresh.offset();
    arena->write_at(offset, data, size);
    region = std::move(fresh);
    return static_cast<uint32_t>(offset / stride);
}

inline void hash_float(uint64_t& h, float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    hash_mix(h, bits);
}

void hash_element(uint64_t& h, IHierarchy& hier, const IObject::Ptr& obj)
{
    if (!obj) return;
    if (::velk::has_attachment<IVisual>(obj)) {
        if (auto elem = interface_pointer_cast<IElement>(obj)) {
            if (auto es = read_state<IElement>(elem.get())) {
                // world_aabb on container elements aggregates all
                // descendant aabbs (including camera / light) so it
                // moves with camera pan. Instead hash each visual's
                // own world-space translation + size — per-element
                // properties that only change when that element or an
                // ancestor transform actually moved the visual in
                // world space.
                hash_float(h, es->world_matrix(0, 3));
                hash_float(h, es->world_matrix(1, 3));
                hash_float(h, es->world_matrix(2, 3));
                hash_float(h, es->size.width);
                hash_float(h, es->size.height);
                hash_float(h, es->size.depth);
            }
        }
    }
    for (auto& kid : hier.children_of(obj)) {
        hash_element(h, hier, kid);
    }
}

} // namespace

uint64_t SceneBvh::hash_visual_aabbs(IScene* scene)
{
    if (!scene) return 0;
    auto* hier = interface_cast<IHierarchy>(scene);
    auto root = scene->root();
    if (!hier || !root) return 0;
    uint64_t h = kFnvBasis;
    hash_element(h, *hier, root);
    return h;
}

void SceneBvh::rebuild(IScene* scene, FrameContext& ctx, bool dirty,
                       ShapeCb shape_cb, void* shape_user)
{
    VELK_PERF_SCOPE("renderer.bvh_rebuild");
    // Second-stage dirty check: the caller's heuristic (visuals in
    // redraw_list) over-triggers because the scene re-adds every
    // element to redraw_list on any Layout dirty flag, including
    // camera pans that don't actually move any visual. Hash each
    // visual element's translation + size; if it matches the last
    // full build, keep the cache.
    uint64_t current_hash = 0;
    if (dirty && !cached_nodes_.empty()) {
        VELK_PERF_SCOPE("renderer.bvh_hash");
        current_hash = hash_visual_aabbs(scene);
        if (current_hash == cached_aabb_hash_) {
            dirty = false;
        }
    }
    bool rebuilt = false;
    if (dirty || cached_nodes_.empty()) {
        VELK_PERF_SCOPE("renderer.bvh_build");
        rebuilt = true;
        auto build = build_scene_bvh(scene, ctx.render_ctx, shape_cb, shape_user);
        cached_nodes_ = std::move(build.nodes);
        cached_shapes_ = std::move(build.shapes);
        cached_mesh_instances_ = std::move(build.mesh_instances);
        root_ = build.root_index;
        node_count_ = static_cast<uint32_t>(cached_nodes_.size());
        shape_count_ = static_cast<uint32_t>(cached_shapes_.size());
        cached_aabb_hash_ = current_hash ? current_hash : hash_visual_aabbs(scene);
        dirty_ = false;
    }

    VELK_PERF_SCOPE("renderer.bvh_upload");
    // Stage the packed mesh-instance array first so we can stamp
    // each mesh shape's `mesh_data_addr` with a stable per-instance
    // address (base + i * sizeof(MeshInstanceData)).
    uint64_t mesh_instances_base = mesh_instances_buffer_.upload(
        cached_mesh_instances_.data(),
        cached_mesh_instances_.size() * sizeof(MeshInstanceData),
        ctx).address;
    for (size_t i = 0; i < cached_shapes_.size() && i < cached_mesh_instances_.size(); ++i) {
        if (cached_shapes_[i].shape_kind != kRtShapeKindMesh) continue;
        cached_shapes_[i].mesh_data_addr =
            mesh_instances_base + i * sizeof(MeshInstanceData);
    }

    // Nodes / shapes live in persistent regions of the Renderer-owned shared
    // arenas, so every scene's BVH holds a distinct region and multiple BVHs
    // in one frame never collide on the set = 1 slot. The region's element
    // base is stamped into FrameGlobals / RtRoot; shaders read
    // data[base + index]. A frame that changes nothing uploads nothing.
    //
    // Shapes are considered changed when the topology was rebuilt or when the
    // mesh-instance array moved (the stamping above rewrites every mesh
    // shape's mesh_data_addr).
    const bool shapes_dirty =
        rebuilt || mesh_instances_base != last_mesh_instances_base_;
    last_mesh_instances_base_ = mesh_instances_base;

    if (shapes_dirty || !shapes_region_.valid()) {
        shape_base_ = upload_region(ctx.bvh_shapes_arena, shapes_region_,
                                    cached_shapes_.data(),
                                    cached_shapes_.size() * sizeof(RtShape),
                                    sizeof(RtShape), ctx);
    }
    if (rebuilt || !nodes_region_.valid()) {
        node_base_ = upload_region(ctx.bvh_nodes_arena, nodes_region_,
                                   cached_nodes_.data(),
                                   cached_nodes_.size() * sizeof(GpuBvhNode),
                                   sizeof(GpuBvhNode), ctx);
    }
}

} // namespace velk::impl
