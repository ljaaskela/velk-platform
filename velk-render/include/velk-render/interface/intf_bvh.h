#ifndef VELK_RENDER_INTF_BVH_H
#define VELK_RENDER_INTF_BVH_H

#include <velk/api/math_types.h>
#include <velk/interface/intf_interface.h>

#include <velk-render/interface/intf_buffer.h>

#include <cstdint>

namespace velk {

/**
 * @brief Hit record for IBvh CPU-side spatial queries.
 *
 * `shape_index` indexes into the implementation's shape list; consumers
 * map it back to whatever domain object emitted the shape (an element,
 * a collider, etc.) via implementation-specific means.
 */
struct BvhHit
{
    float    t = 0.f;
    uint32_t shape_index = 0;
};

/**
 * @brief Generic spatial index over a set of shapes.
 *
 * Implementations build and maintain a bounding-volume hierarchy over
 * some shape source (a full scene, a subtree, an element subset). The
 * interface exposes a CPU-side query API for consumers that need hit
 * testing (picking, collision, drag hit-tests) and GPU-buffer accessors
 * for the renderer's ray-traced / deferred paths.
 *
 * Multiple IBvh instances can coexist as attachments on the same scene
 * root (e.g. a primary render BVH alongside a coarser picking BVH).
 * Callers that want the primary BVH attached to a scene should use
 * IScene::get_default_bvh() rather than iterating attachments.
 */
class IBvh : public Interface<IBvh>
{
public:
    /**
     * @brief Tests whether any shape is intersected by the ray within
     *        @p t_max. Early-exits on the first confirmed hit.
     */
    virtual bool any_hit(vec3 origin, vec3 dir, float t_max) const = 0;

    /**
     * @brief Finds the closest shape intersected by the ray, if any.
     *
     * Returns true and populates @p out on hit; returns false when the
     * ray misses. Implementations are free to ignore t-values below a
     * small epsilon to avoid self-intersection.
     */
    virtual bool closest_hit(vec3 origin, vec3 dir, BvhHit& out) const = 0;

    /**
     * @brief Index of the BVH's root node within the nodes buffer.
     */
    virtual uint32_t get_root_index() const = 0;

    /**
     * @brief Number of nodes currently live in the BVH.
     */
    virtual uint32_t get_node_count() const = 0;

    /**
     * @brief Number of shapes currently indexed by the BVH.
     */
    virtual uint32_t get_shape_count() const = 0;

    /**
     * @brief Element base to add to node / shape indices this frame.
     *
     * The BVH nodes / shapes live in IGpuArena ring regions; the base is
     * the region's element offset for the current frame. Shaders read
     * `data[base + index]`; the renderer stamps these into FrameGlobals /
     * RtRoot. Valid only for the frame in which the BVH was last rebuilt.
     */
    virtual uint32_t get_node_base() const = 0;
    virtual uint32_t get_shape_base() const = 0;

    /**
     * @brief Marks the BVH as stale so the next access rebuilds.
     *
     * Implementations rebuild lazily: `invalidate()` only flags the
     * instance; the actual rebuild happens on the next query or GPU
     * buffer read.
     */
    virtual void invalidate() = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_BVH_H
