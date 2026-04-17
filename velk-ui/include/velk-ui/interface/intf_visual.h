#ifndef VELK_UI_INTF_VISUAL_H
#define VELK_UI_INTF_VISUAL_H

#include <velk/api/math_types.h>
#include <velk/api/object_ref.h>
#include <velk/interface/intf_metadata.h>
#include <velk/vector.h>

#include <velk/string_view.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/render_types.h>
#include <velk-ui/interface/intf_trait.h>

namespace velk::ui {

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
 * @brief Visual representation attached to an element.
 *
 * Defines how an element appears on screen. An element can have one or more
 * IVisual attachments. The renderer iterates them and draws what they produce.
 */
class IVisual : public Interface<IVisual, ITrait>
{
public:
    VELK_INTERFACE(
        (PROP, ::velk::color, color, {}),
        (PROP, ObjectRef, paint, {}),
        (PROP, VisualPhase, visual_phase, VisualPhase::BeforeChildren)
    )

    /** @brief Returns draw entries for this visual within the given bounds. */
    virtual vector<DrawEntry> get_draw_entries(const rect& bounds) = 0;

    /**
     * @brief Returns GPU resources used by this visual that need uploading
     *        and lifetime tracking by the renderer.
     *
     * Returned by value (not array_view) so the visual is free to construct
     * the list however it likes (member, fresh allocation, conditional
     * inclusion) without committing to backing storage, and so the renderer
     * sees a stable snapshot with no aliasing into visual-internal state.
     *
     * The default returns an empty vector for visuals that have no GPU
     * resources (rect, rounded rect, gradient, etc.).
     */
    virtual vector<IBuffer::Ptr> get_gpu_resources() const { return {}; }

    /**
     * @brief Returns the pipeline key for this visual's built-in shader,
     *        or 0 if the visual does not use a built-in pipeline (e.g. it
     *        brings its own material via the paint property).
     *
     * Each visual class should return a stable, class-unique 64-bit value
     * (typically a constexpr hash of a class-level name). The renderer
     * compiles a pipeline per unique key on first sight.
     */
    virtual uint64_t get_pipeline_key() const = 0;

    /**
     * @brief Returns the vertex shader source for this visual's pipeline.
     *        An empty string means "use the registered default vertex shader".
     */
    virtual string_view get_vertex_src() const = 0;

    /**
     * @brief Returns the fragment shader source for this visual's pipeline.
     *        An empty string means "use the registered default fragment shader".
     */
    virtual string_view get_fragment_src() const = 0;

    /**
     * @brief Returns a GLSL snippet that intersects a ray with this visual's
     *        shape, for use by the ray-traced render backend.
     *
     * The snippet is a function definition (not a whole shader). Currently
     * unused by the RT path — rect / cube / sphere intersects live in the
     * compute shader prelude and dispatch on get_shape_kind(). Reserved for
     * visuals with a genuinely one-off primitive (rounded box, capsule, SDF
     * blob, etc.) that would bloat the prelude with a single-use function.
     */
    virtual string_view get_intersect_src() const = 0;

    /**
     * @brief Returns this visual's shape kind for the RT dispatch.
     *
     *   0 = rect (planar quad, u_axis x v_axis)
     *   1 = cube (oriented box, u_axis x v_axis x w_axis)
     *   2 = sphere (centered in the element's bounding box, radius from size)
     *
     * The RT compute shader switches on this per-shape value to call the
     * right intersect routine in the prelude. Default is rect.
     */
    virtual uint32_t get_shape_kind() const { return 0; }
};

} // namespace velk::ui

#endif // VELK_UI_INTF_VISUAL_H
