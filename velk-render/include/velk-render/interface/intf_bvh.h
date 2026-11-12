#ifndef VELK_RENDER_INTF_BVH_H
#define VELK_RENDER_INTF_BVH_H

#include <velk/api/math_types.h>
#include <velk/interface/intf_interface.h>

#include <velk-render/gpu_data.h>
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

    /** @brief How rays follow motion it does not hold, and how far that reaches, or empty. */
    virtual string_view get_ray_motion(float& reach) const = 0;

    /**
     * @brief Where what it holds changed in its last update: the world bounds of each thing
     *        that moved, before and after, appeared or went away.
     *
     * For whatever keeps results of rays, to know which to trace again. Valid until the
     * next update.
     *
     * @param all Set if everything may have changed, in which case the list does not say.
     */
    virtual array_view<const WorldBox> changed_bounds(bool& all) const = 0;

    /**
     * @brief Where each object it holds is, by object id, for whatever keeps data by object.
     *
     * Returns the word in the culling arena (IRenderBackend::kGlobalCulling) where the
     * table starts, or 0xFFFFFFFF if there is none yet, and sets @p slots to how many it
     * has. Entry `id & 0x3FFFFFF` is 14 words: the object's object space to the world by
     * rows (3 rows of 4 floats, the 4th the world position of its origin), then its id's
     * generation plus one, or 0 if the entry is unknown, then the radius of its bounds in
     * world units as a float. An entry whose generation is not
     * the id's is another object's. Objects drawn but not in the BVH have none. Valid
     * after the update.
     */
    virtual uint32_t get_object_transforms(uint32_t& slots) const = 0;

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
