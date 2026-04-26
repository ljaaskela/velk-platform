#ifndef VELK_UI_INTF_VISUAL_H
#define VELK_UI_INTF_VISUAL_H

#include <velk/api/math_types.h>
#include <velk/api/object_ref.h>
#include <velk/interface/intf_metadata.h>
#include <velk/vector.h>

#include <velk/string_view.h>
#include <velk-render/interface/intf_buffer.h>
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
    // role interfaces (`IRasterShader`, `IAnalyticShape`). A visual
    // that contributes its own pipeline implements `IRasterShader`;
    // a visual that's an analytic RT primitive implements
    // `IAnalyticShape`. Visuals can implement either, both, or
    // neither (e.g. a TextureVisual that piggybacks on a paint-
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

} // namespace velk

#endif // VELK_UI_INTF_VISUAL_H
