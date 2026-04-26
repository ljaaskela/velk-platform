#ifndef VELK_UI_SCENE_BVH_H
#define VELK_UI_SCENE_BVH_H

#include "frame_data_manager.h"
#include "scene_collector.h"

#include <velk/api/velk.h>
#include <velk/ext/object.h>

#include <velk-render/interface/intf_bvh.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/**
 * @brief IBvh implementation backed by an IScene tree walk.
 *
 * Attached to the scene root on first use. Owns the current BVH build
 * state (nodes + shapes byte arrays, root index, counts). The renderer
 * calls `rebuild` each frame with a per-shape callback that resolves
 * materials / textures / custom intersects; the callback still lives
 * in the renderer because it needs access to renderer-owned registries.
 *
 * M2: rebuild runs every frame and uploads into a `FrameDataManager`
 * (same perf profile as before). M3 will migrate to persistent IBuffer
 * instances with dirty tracking so static frames skip the rebuild.
 */
namespace impl {

class SceneBvh : public ::velk::ext::ObjectCore<SceneBvh, ::velk::IBvh>
{
public:
    VELK_CLASS_UID(::velk::ui::ClassId::SceneBvh, "SceneBvh");

    SceneBvh() = default;

    /**
     * @brief Rebuilds the BVH from the scene tree, writing nodes and
     *        shapes into @p frame_buffer to obtain GPU addresses for
     *        this frame. When @p dirty is false and a cached build
     *        from a previous frame exists, the tree walk is skipped
     *        and the cached arrays are reused; only the frame-buffer
     *        memcpy runs. Callers compute @p dirty from scene-level
     *        change signals (redraw / removed lists).
     *
     * The callback fires per emitted shape during a full rebuild so
     * the caller can fill material / texture / intersect fields.
     * Clean-frame reuse relies on those fields remaining valid across
     * frames (materials with persistent data buffers, stable
     * TextureIds) — scenes that invalidate those fields must force
     * @p dirty = true.
     */
    template <typename F>
    void rebuild(IScene* scene, IRenderContext* ctx, FrameDataManager& frame_buffer, bool dirty,
                 F&& shape_cb);

    /// Frame-local GPU addresses. Only valid for the frame in which
    /// `rebuild` was called.
    uint64_t nodes_addr() const { return nodes_addr_; }
    uint64_t shapes_addr() const { return shapes_addr_; }

    // IBvh
    bool any_hit(vec3 /*origin*/, vec3 /*dir*/, float /*t_max*/) const override
    {
        // TODO(m-later): wire CPU-side traversal over cached_nodes_ /
        // cached_shapes_ so app-side pickers / collision consumers can
        // query without going through GPU. Stub until then.
        return false;
    }

    bool closest_hit(vec3 /*origin*/, vec3 /*dir*/, BvhHit& /*out*/) const override
    {
        return false;
    }

    IBuffer::Ptr get_nodes_buffer() const override
    {
        // TODO(m3): expose the persistent nodes IBuffer once the arena
        // migration lands. Today the BVH lives in the per-frame buffer
        // and the renderer reads `nodes_addr()` directly.
        return nullptr;
    }

    IBuffer::Ptr get_shapes_buffer() const override { return nullptr; }

    uint32_t get_root_index() const override { return root_; }
    uint32_t get_node_count() const override { return node_count_; }
    uint32_t get_shape_count() const override { return shape_count_; }

    void invalidate() override { dirty_ = true; }

    /// Clears the cached topology so the next `rebuild` re-walks every
    /// shape and re-fires the shape callback, regardless of the AABB
    /// hash check. Used by debug hooks that need to observe per-shape
    /// emit-time state on demand.
    void force_full_rebuild() { cached_nodes_.clear(); }

private:
    /// Hash all visual-bearing elements' world_aabbs to detect
    /// geometry-only changes without re-emitting shapes. Walks the
    /// scene tree once per call; O(#elements) regardless of emitted
    /// shape count.
    static uint64_t hash_visual_aabbs(IScene* scene);

    // Cached build output, kept across frames. Reused when the caller
    // signals the scene is clean.
    vector<GpuBvhNode> cached_nodes_;
    vector<RtShape>    cached_shapes_;
    /// Parallel to cached_shapes_: per-shape MeshInstanceData payload
    /// (zeroed for non-mesh kinds). Re-uploaded each frame so the
    /// mesh_data_addr GPU pointer stays valid as the per-frame buffer
    /// rotates. The instance struct holds the per-element world
    /// matrices plus a stable pointer to the mesh-owned static buffer
    /// (resolved once by the renderer's BVH callback during a fresh
    /// rebuild).
    vector<MeshInstanceData> cached_mesh_instances_;
    uint64_t cached_aabb_hash_ = 0;  ///< Hash of visual-aabbs at last build.

    uint64_t nodes_addr_ = 0;
    uint64_t shapes_addr_ = 0;
    uint32_t root_ = 0;
    uint32_t node_count_ = 0;
    uint32_t shape_count_ = 0;
    bool dirty_ = true;
};

template <typename F>
void SceneBvh::rebuild(IScene* scene, IRenderContext* ctx, FrameDataManager& frame_buffer,
                        bool dirty, F&& shape_cb)
{
    // Second-stage dirty check: the caller's heuristic (visuals in
    // redraw_list) can over-trigger because the scene re-adds every
    // element to redraw_list on any Layout change (including camera
    // pans that don't touch geometry). Cross-check by hashing visual
    // elements' world_aabbs; if the hash matches our last build, the
    // cache is still correct and we can skip the rebuild.
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
        auto build = build_scene_bvh(scene, ctx, std::forward<F>(shape_cb));
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

} // namespace impl

using SceneBvh = impl::SceneBvh;

} // namespace velk::ui

#endif // VELK_UI_SCENE_BVH_H
