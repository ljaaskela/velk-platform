#include "scene_bvh.h"

#include <velk/api/state.h>
#include <velk/interface/intf_hierarchy.h>
#include <velk/interface/intf_object_storage.h>

#include <velk-scene/interface/intf_element.h>
#include <velk-scene/interface/intf_visual.h>

#include <cstring>

namespace velk::impl {

namespace {

constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnvPrime = 0x100000001b3ULL;

inline void hash_mix(uint64_t& h, uint64_t v)
{
    h = (h ^ v) * kFnvPrime;
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

void SceneBvh::rebuild(IScene* scene, IRenderContext* ctx, FrameDataManager& frame_buffer,
                        bool dirty, ShapeCb shape_cb, void* shape_user)
{
    // Second-stage dirty check: the caller's heuristic (visuals in
    // redraw_list) over-triggers because the scene re-adds every
    // element to redraw_list on any Layout dirty flag, including
    // camera pans that don't actually move any visual. Hash each
    // visual element's translation + size; if it matches the last
    // full build, keep the cache.
    uint64_t current_hash = 0;
    if (dirty && !cached_nodes_.empty()) {
        current_hash = hash_visual_aabbs(scene);
        if (current_hash == cached_aabb_hash_) {
            dirty = false;
        }
    }
    if (dirty || cached_nodes_.empty()) {
        auto build = build_scene_bvh(scene, ctx, shape_cb, shape_user);
        cached_nodes_ = std::move(build.nodes);
        cached_shapes_ = std::move(build.shapes);
        cached_mesh_instances_ = std::move(build.mesh_instances);
        root_ = build.root_index;
        node_count_ = static_cast<uint32_t>(cached_nodes_.size());
        shape_count_ = static_cast<uint32_t>(cached_shapes_.size());
        cached_aabb_hash_ = current_hash ? current_hash : hash_visual_aabbs(scene);
        dirty_ = false;
    }

    // Re-upload per-shape MeshInstanceData each frame and patch the
    // cached shapes' mesh_data_addr to point at this frame's buffer
    // copy. The shape callback only fires on a dirty rebuild, so the
    // address it stamped was only valid for that one frame; without
    // this re-stamp pass the next frame's shadow / RT shaders would
    // dereference a stale pointer into a recycled buffer. (The
    // mesh-static buffer pointer inside MeshInstanceData is stable
    // across frames so we don't have to re-resolve it here.)
    for (size_t i = 0; i < cached_shapes_.size() && i < cached_mesh_instances_.size(); ++i) {
        if (cached_shapes_[i].shape_kind != kRtShapeKindMesh) continue;
        cached_shapes_[i].mesh_data_addr = frame_buffer.write(
            &cached_mesh_instances_[i], sizeof(MeshInstanceData));
    }

    // Always re-publish into the current frame buffer. Nodes + shapes
    // are the same bytes as the last frame (when clean), so the memcpy
    // is cheap; the frame-buffer address itself is per-frame so we
    // must stamp globals with this frame's copy.
    nodes_addr_ = 0;
    shapes_addr_ = 0;
    if (!cached_nodes_.empty()) {
        nodes_addr_ = frame_buffer.write(
            cached_nodes_.data(), cached_nodes_.size() * sizeof(GpuBvhNode));
    }
    if (!cached_shapes_.empty()) {
        shapes_addr_ = frame_buffer.write(
            cached_shapes_.data(), cached_shapes_.size() * sizeof(RtShape));
    }
}

} // namespace velk::impl
