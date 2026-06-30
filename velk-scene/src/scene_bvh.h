#ifndef VELK_UI_SCENE_BVH_H
#define VELK_UI_SCENE_BVH_H

#include "scene_collector.h"

#include <velk/api/velk.h>
#include <velk/ext/object.h>

#include <velk-render/ext/persistent_buffer.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_bvh.h>
#include <velk-render/interface/intf_gpu_arena.h>
#include <velk-render/render_path/frame_context.h>
#include <velk-scene/plugin.h>

namespace velk {

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
    VELK_CLASS_UID(::velk::ClassId::SceneBvh, "SceneBvh");

    SceneBvh() = default;

    /**
     * @brief Rebuilds the BVH from the scene tree, staging nodes /
     *        shapes / mesh-instance data into per-bvh persistent
     *        IBuffers so GPU addresses are stable across frames.
     *        write_diff gates the upload (and the implicit address
     *        change) on real byte change. When @p dirty is false and a
     *        cached build exists, the tree walk is skipped and the
     *        cached arrays are reused; the per-frame work reduces to
     *        memcmp.
     *
     * The callback fires per emitted shape during a full rebuild so
     * the caller can fill material / texture / intersect fields.
     * Clean-frame reuse relies on those fields remaining valid across
     * frames (materials with persistent data buffers, stable
     * TextureIds) — scenes that invalidate those fields must force
     * @p dirty = true.
     */
    void rebuild(IScene* scene, FrameContext& ctx, bool dirty,
                 ShapeCb shape_cb, void* shape_user);

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

    uint32_t get_root_index() const override { return root_; }
    uint32_t get_node_count() const override { return node_count_; }
    uint32_t get_shape_count() const override { return shape_count_; }
    uint32_t get_node_base() const override { return node_base_; }
    uint32_t get_shape_base() const override { return shape_base_; }

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

    /// Nodes / shapes are written into the Renderer-owned shared IGpuArenas
    /// (set = 1) each rebuild; this BVH gets a suballocated region whose base
    /// is stamped into FrameGlobals / RtRoot. The mesh-instance array stays a
    /// PersistentBuffer (BDA) until the mesh chain migrates.
    PersistentBuffer mesh_instances_buffer_;

    uint64_t nodes_addr_ = 0;
    uint64_t shapes_addr_ = 0;
    uint32_t root_ = 0;
    uint32_t node_count_ = 0;
    uint32_t shape_count_ = 0;
    uint32_t node_base_ = 0;   ///< Ring-region element base for nodes this frame.
    uint32_t shape_base_ = 0;  ///< Ring-region element base for shapes this frame.
    bool dirty_ = true;
};

} // namespace impl

using SceneBvh = impl::SceneBvh;

} // namespace velk

#endif // VELK_UI_SCENE_BVH_H
