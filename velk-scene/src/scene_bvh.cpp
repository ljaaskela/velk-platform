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
                       uint64_t size, uint64_t stride)
{
    if (!arena || size == 0) {
        region = {};
        return 0;
    }
    auto fresh = arena->alloc(size);
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
                // Each visual's own world matrix and size only change
                // when that element or an ancestor actually moved,
                // rotated or scaled the visual in world space.
                for (int i = 0; i < 16; ++i) {
                    hash_float(h, es->world_matrix.m[i]);
                }
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

void SceneBvh::update_probe_volume(uint32_t frame)
{
    // Growing past the volume adds this fraction of its size as margin, so an
    // object moving outward does not reset the probes every frame.
    constexpr float kGrowMargin = 0.125f;
    // Bounds shrinking below this fraction of the volume on any axis re-fit it,
    // since a loose volume spreads the probes thinly.
    constexpr float kRefitBelow = 0.5f;

    if (cached_nodes_.empty()) return;
    const auto& root = cached_nodes_[root_];
    const float* new_min = root.aabb_min;
    const float* new_max = root.aabb_max;
    if (new_min[0] > new_max[0]) return;  // Empty BVH: inverted root, nothing to cover.

    const float bounds_min[3] = {new_min[0], new_min[1], new_min[2]};
    const float bounds_max[3] = {new_max[0], new_max[1], new_max[2]};
    float vol_min[3] = {probe_volume_.min.x, probe_volume_.min.y, probe_volume_.min.z};
    float vol_max[3] = {probe_volume_.max.x, probe_volume_.max.y, probe_volume_.max.z};

    bool refit = !probe_volume_placed_;
    bool escaped = false;
    for (int i = 0; i < 3; ++i) {
        escaped = escaped || bounds_min[i] < vol_min[i] || bounds_max[i] > vol_max[i];
        refit = refit || (bounds_max[i] - bounds_min[i]) < kRefitBelow * (vol_max[i] - vol_min[i]);
    }
    if (!refit && !escaped) return;

    for (int i = 0; i < 3; ++i) {
        if (refit) {
            vol_min[i] = bounds_min[i];
            vol_max[i] = bounds_max[i];
        } else {
            const float margin = kGrowMargin * (bounds_max[i] - bounds_min[i]);
            if (bounds_min[i] < vol_min[i]) vol_min[i] = bounds_min[i] - margin;
            if (bounds_max[i] > vol_max[i]) vol_max[i] = bounds_max[i] + margin;
        }
    }
    probe_volume_.min = {vol_min[0], vol_min[1], vol_min[2]};
    probe_volume_.max = {vol_max[0], vol_max[1], vol_max[2]};
    probe_volume_.reset_frame = frame;
    probe_volume_placed_ = true;
}

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
    // redraw_list) also fires for changes that leave geometry alone,
    // such as a material change. Hash each visual element's world
    // matrix + size; if it matches the last full build, keep the cache.
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
        update_probe_volume(static_cast<uint32_t>(ctx.present_counter));
    }

    VELK_PERF_SCOPE("renderer.bvh_upload");
    // Nodes / shapes / mesh instances live in persistent regions of the
    // Renderer-owned shared arenas, so every scene's BVH holds a distinct
    // region and multiple BVHs in one frame never collide on the set = 1 slot.
    // The region's element base is stamped into FrameGlobals / RtRoot; shaders
    // read data[base + index]. A frame that changes nothing uploads nothing.
    //
    // Mesh instances go first: their base is stamped into each mesh shape, so
    // it has to be settled before the shape array is uploaded. The array only
    // changes when the build does (the renderer's callback fills it during a
    // fresh walk), so a stable build keeps both the region and its bytes.
    bool shapes_dirty = rebuilt;
    if (rebuilt || !mesh_instances_region_.valid()) {
        const uint32_t base = upload_region(
            ctx.mesh_instances_arena, mesh_instances_region_,
            cached_mesh_instances_.data(),
            cached_mesh_instances_.size() * sizeof(MeshInstanceData),
            sizeof(MeshInstanceData));
        // A moved base rewrites every mesh shape's index, so the shape region
        // has to follow it even on a frame that did not rebuild.
        if (base != mesh_instance_base_) shapes_dirty = true;
        mesh_instance_base_ = base;
        for (size_t i = 0; i < cached_shapes_.size() && i < cached_mesh_instances_.size(); ++i) {
            if (cached_shapes_[i].shape_kind != kRtShapeKindMesh) continue;
            cached_shapes_[i].mesh_instance_base = base + static_cast<uint32_t>(i);
        }
    }

    if (shapes_dirty || !shapes_region_.valid()) {
        shape_base_ = upload_region(ctx.bvh_shapes_arena, shapes_region_,
                                    cached_shapes_.data(),
                                    cached_shapes_.size() * sizeof(RtShape),
                                    sizeof(RtShape));
    }
    if (rebuilt || !nodes_region_.valid()) {
        node_base_ = upload_region(ctx.bvh_nodes_arena, nodes_region_,
                                   cached_nodes_.data(),
                                   cached_nodes_.size() * sizeof(GpuBvhNode),
                                   sizeof(GpuBvhNode));
    }
}

} // namespace velk::impl
