#ifndef VELK_UI_INTF_VISUAL_H
#define VELK_UI_INTF_VISUAL_H

#include <velk/api/math_types.h>
#include <velk/api/object_ref.h>
#include <velk/array_view.h>
#include <velk/interface/intf_metadata.h>
#include <velk/vector.h>

#include <velk/string_view.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_mesh.h>
#include <velk-render/render_types.h>
#include <velk-scene/interface/intf_trait.h>

namespace velk {
class IRenderContext;
}

namespace velk {

/** @brief Controls when a visual draws relative to an element's children. */
enum class VisualPhase : uint8_t
{
    /** @brief Draw the visual before any of the children of the Element the visual is attached to.
               This is the default and the correct choice for most use cases. */
    BeforeChildren = 0,
    /** @brief Draw the visual after drawing the child hierarchy of the Element the Visual is attached to.
               A typical use case would be a border, overlay, focus indicator. */
    AfterChildren,
};

/**
 * @brief Renderer-facing contract for a visual attached to an element.
 *
 * IVisual is the trait-to-GPU contract: the renderer iterates visuals and
 * calls these methods to obtain draw entries, local bounds, and GPU
 * resources. Every method that runs during rendering receives the
 * IRenderContext, so visuals never need to cache it.
 *
 * Concrete visuals implement either `IVisual2D` (2D authoring affordances
 * like color / paint / visual_phase) or `IVisual3D` (3D authoring
 * affordances, currently empty). Both inherit IVisual so the renderer
 * can iterate them uniformly without branching on kind.
 */
class IVisual : public Interface<IVisual, ITrait>
{
public:
    /** @brief Returns draw entries for this visual within the given local size.
               The visual is free to fetch meshes, materials, or other
               renderer-owned resources via @p ctx; the returned DrawEntry's
               `mesh` field carries whatever mesh the visual wants bound. */
    virtual vector<DrawEntry> get_draw_entries(::velk::IRenderContext& ctx,
                                               const ::velk::size& bounds) = 0;

    /**
     * @brief Returns the local-space bounds of what this visual
     *        actually renders within the element's @p bounds.
     *
     * Most visuals render inside the element's layout box and return
     * an aabb matching it. Visuals whose output can extend past that
     * box (text with overflowing glyphs, drop shadows, outline strokes)
     * return the true extent so the element's world_aabb — used for
     * culling, hit-testing, and ray-traced shadow casting — covers
     * everything the visual draws.
     *
     * Called by the layout solver, not the renderer, so it receives no
     * IRenderContext: anything that needs renderer state must be cached
     * on the visual or deferred to `get_draw_entries`.
     */
    virtual aabb get_local_bounds(const ::velk::size& bounds) const = 0;

    /**
     * @brief Returns GPU resources used by this visual that need uploading
     *        and lifetime tracking by the renderer.
     *
     * Returned by value (not array_view) so the visual is free to construct
     * the list however it likes (member, fresh allocation, conditional
     * inclusion) without committing to backing storage, and so the renderer
     * sees a stable snapshot with no aliasing into visual-internal state.
     */
    virtual vector<IBuffer::Ptr> get_gpu_resources(::velk::IRenderContext& ctx) const = 0;

    // Shader provision and analytic-shape dispatch live on separate
    // role interfaces (`IShaderSource`, `IAnalyticShape`). A visual
    // that contributes its own pipeline / discard snippet implements
    // `IShaderSource`; a visual that's an analytic RT primitive
    // implements `IAnalyticShape`. Visuals can implement either, both,
    // or neither (e.g. a TextureVisual that piggybacks on a paint-
    // supplied material).
};

/**
 * @brief 2D visual authoring contract.
 *
 * Carries the app-facing properties that make sense for a visual filling
 * a layout box: a tint color, an optional material paint, and the
 * BeforeChildren / AfterChildren draw phase. Rect, rounded rect, text,
 * image, and texture visuals all implement this.
 */
class IVisual2D : public Interface<IVisual2D, IVisual>
{
public:
    VELK_INTERFACE(
        (PROP, ::velk::color, color, {}),
        (PROP, ObjectRef, paint, {}),
        (PROP, VisualPhase, visual_phase, VisualPhase::BeforeChildren)
    )
};

/**
 * @brief 3D visual authoring contract.
 *
 * Carries a reference to the `IMesh` the visual draws. Materials live
 * on the mesh's primitives, not on the visual — callers set material
 * via `mesh->get_primitives()[idx]->material` (typically wrapped by
 * the API `MeshPrimitive::set_material`).
 *
 * Procedural primitives (cube, sphere) lazily populate this slot from
 * the render context's mesh builder on first draw when the caller
 * hasn't set one explicitly, so the mesh becomes observable and its
 * primitives' materials can be set per-instance.
 */
class IVisual3D : public Interface<IVisual3D, IVisual>
{
public:
    VELK_INTERFACE(
        (PROP, ObjectRef, mesh, {})
    )
};

/// An instance's placement relative to its element: the first three rows of its matrix.
struct InstanceTransform
{
    float rows[3][4];

    static InstanceTransform from(const mat4& m)
    {
        InstanceTransform t;
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 4; ++col) {
                t.rows[row][col] = m(row, col);
            }
        }
        return t;
    }

    mat4 to_mat4() const
    {
        mat4 m = mat4::identity();
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 4; ++col) {
                m(row, col) = rows[row][col];
            }
        }
        return m;
    }
};

/**
 * @brief Draws meshes many times, on one element.
 *
 * For counts where an element per instance is too expensive. Each instance uses one
 * mesh: IVisual3D::mesh is mesh 0, and `add_mesh` adds more, such as species of a tree.
 * Transform traits on the element move all instances together. Each instance has a
 * slot, which is its stable index until it is removed, and removed slots are reused.
 *
 * Not drawn by the RT path's primary rays yet, which walk a flat list of shapes. Its
 * bounces and shadows do see instances. Under a render target, which has no GPU culling,
 * every instance draws as mesh 0.
 */
class IInstancedMeshVisual : public Interface<IInstancedMeshVisual>
{
public:
    /// Adds a mesh instances can use and returns its index.
    virtual uint32_t add_mesh(const IMesh::Ptr& mesh) = 0;
    /// Adds an instance of @p mesh and returns its slot.
    virtual uint32_t add_instance(const mat4& transform, uint32_t mesh = 0) = 0;
    /// Adds instances of @p mesh. Slots are consecutive from the returned one if no slot
    /// has been removed.
    virtual uint32_t add_instances(array_view<const InstanceTransform> transforms, uint32_t mesh = 0) = 0;
    virtual void set_transform(uint32_t slot, const mat4& transform) = 0;
    virtual void remove_instance(uint32_t slot) = 0;
    /// Live instances.
    virtual uint32_t get_instance_count() const = 0;
    /// Instances further from the camera than this are not drawn, though rays still
    /// see them. 0, the default, means no limit.
    virtual void set_draw_distance(float distance) = 0;
    /// Whether instances too small on screen are averaged into the pixels they cover, and
    /// wide rays average over them too, rather than each being drawn and tested. This
    /// avoids flicker and is far cheaper. On by default. Applies to the deferred path.
    virtual void set_far_form(bool enabled) = 0;

    /**
     * @brief Moves instances on the GPU, for motion that stays near where each instance
     *        was placed, such as wind in trees.
     *
     * @p glsl defines `mat4 velk_instance_motion(mat4 rest, uint slot, vec4 params)`,
     * which returns an instance's world matrix given the one it was placed with. It runs
     * for every instance each frame the params or placements change, with no CPU work per
     * instance and no change to any tree, and rays and raster see the same result.
     *
     * No point of a mesh may move further than @p max_displacement, since the instance's
     * bounds in the BVH are grown by that much. Going past it is logged as an error, and
     * rays may then miss the instance. An empty @p glsl removes the motion.
     */
    virtual void set_motion(string_view glsl, float max_displacement) = 0;
    /// The `params` passed to the motion function, such as the current time.
    virtual void set_motion_params(const vec4& params) = 0;
};

class IGpuArena;
class IGpuResourceManager;

/** Internal half of an instanced mesh visual, read by the renderer and the BVH. */
class IInstancedMeshInternal : public Interface<IInstancedMeshInternal>
{
public:
    /// Transforms by slot, including the slots of removed instances.
    virtual array_view<const InstanceTransform> get_transforms() const = 0;
    virtual bool is_live(uint32_t slot) const = 0;

    /// Brings the raster instance data in @p arena up to date, given the element's
    /// @p world matrix.
    virtual void flush_instances(IGpuArena& arena, IGpuResourceManager& resources, const mat4& world) = 0;
    virtual uint64_t instance_region_offset() const = 0;
    virtual uint64_t instance_region_size() const = 0;

    virtual uint32_t get_mesh_count() const = 0;
    virtual IMesh::Ptr get_mesh(uint32_t index) const = 0;
    /// The mesh the instance in @p slot uses.
    virtual uint32_t get_slot_mesh(uint32_t slot) const = 0;
    /// Live instances of mesh @p index.
    virtual uint32_t get_mesh_instance_count(uint32_t index) const = 0;
    /// Mesh @p index's bounding sphere, as center xyz and radius.
    virtual void get_cull_sphere(uint32_t index, float out[4]) const = 0;
    virtual float get_draw_distance() const = 0;
    virtual bool get_far_form() const = 0;
    /// Mesh @p index's mean projected area at scale 1, the area-weighted colour of its
    /// materials, and its surface area by which of kSurfaceDirections it faces. An area of
    /// 0 means its instances have no far form.
    virtual void get_far_look(uint32_t index, float& area, float albedo[3], float* direction_area) const = 0;

    /// The motion function's source, empty when there is none.
    virtual string_view get_motion_source() const = 0;
    virtual float get_max_displacement() const = 0;
    /// Byte offset in the culling arena of the slots' resting world matrices, as an
    /// InstanceTransform each, and their size. Only with a motion function.
    virtual uint64_t rest_region_offset() const = 0;
    virtual uint64_t rest_region_size() const = 0;
    /// Fills @p out with the params, and returns whether they changed since the last call.
    virtual bool take_motion_params(float out[4]) = 0;
    /// Whether resting matrices were written since the last call.
    virtual bool take_rest_changed() = 0;

    /// Slots changed since the last call, added to @p changed. Sets @p all instead when
    /// everything is to be treated as changed, such as on a new motion function.
    virtual void take_changed_slots(vector<uint32_t>& changed, bool& all) = 0;

    /// Where culling finds this visual's partitions in the culling arena, set by the BVH
    /// that holds them. See `instance_partition_cull_src`. A count of 0 means none.
    virtual void set_partition_cull(uint32_t table_word, uint32_t count, uint32_t list_word) = 0;
    virtual void get_partition_cull(uint32_t& table_word, uint32_t& count, uint32_t& list_word) const = 0;
};

} // namespace velk

#endif // VELK_UI_INTF_VISUAL_H
