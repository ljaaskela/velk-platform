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
 * @brief World-space box the GI probe lattice covers.
 *
 * Stays put while geometry moves inside it, so probes keep their history.
 * `reset_frame` is the frame it last changed, from which probes start over.
 */
struct ProbeVolume
{
    vec3     min{};
    vec3     max{};
    uint32_t reset_frame = 0;
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

    /** @brief The box the GI probe lattice covers for this BVH's shapes. */
    virtual ProbeVolume get_probe_volume() const = 0;

    /** @brief Whether any instances in it have a far form, for rays too wide to resolve them. */
    virtual bool has_medium() const = 0;

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
