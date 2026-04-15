#ifndef VELK_UI_INTF_RENDERER_H
#define VELK_UI_INTF_RENDERER_H

#include <velk/api/math_types.h>
#include <velk/interface/intf_metadata.h>
#include <velk/vector.h>

#include <velk-render/interface/intf_window_surface.h>
#include <velk-ui/interface/intf_element.h>

#include <cstdint>

namespace velk::ui {

/** @brief Describes which cameras to render for a given surface. */
struct ViewDesc
{
    IWindowSurface::Ptr surface;          ///< Target surface.
    vector<IElement::Ptr> cameras;  ///< Cameras to render. Empty = all cameras for this surface.
};

/** @brief Describes what to render in a single prepare() call. */
struct FrameDesc
{
    vector<ViewDesc> views;  ///< Surfaces to render. Empty = all registered views.
};

/** @brief Opaque handle to a prepared frame, returned by prepare() and consumed by present(). */
struct Frame
{
    uint64_t id = 0;  ///< Internal slot identifier. Zero = invalid.
};

/**
 * @brief Scene renderer / dispatcher.
 *
 * Walks scene trees, collects draw entries from visuals, batches them,
 * writes GPU buffers, and submits draw calls to the render backend.
 *
 * Views are defined by camera elements (elements with an ICamera trait).
 * Each view binds a camera to a surface. The camera's element provides
 * the scene (via get_scene()) and the view-projection matrix.
 *
 * Rendering can be split into two phases for threaded use:
 *   Frame f = renderer->prepare();  // Main thread: build draw commands
 *   renderer->present(f);           // Render thread: submit to GPU
 *
 * Or called as a single step:
 *   renderer->render();             // Equivalent to present(prepare())
 *
 * Created via velk_ui::create_renderer(ctx).
 */
class IRenderer : public Interface<IRenderer>
{
public:
    /**
     * @brief Adds a view: renders the camera element's scene onto the surface.
     * @param camera_element Element with an ICamera trait attached.
     * @param surface        Render target surface.
     * @param viewport       Normalized viewport (0..1). Default (all zeros) = full surface.
     */
    virtual void add_view(const IElement::Ptr& camera_element,
                          const IWindowSurface::Ptr& surface,
                          const rect& viewport = {}) = 0;

    /**
     * @brief Removes a previously added view.
     * @param camera_element The camera element used in add_view.
     * @param surface        The surface used in add_view.
     */
    virtual void remove_view(const IElement::Ptr& camera_element,
                             const IWindowSurface::Ptr& surface) = 0;

    /**
     * @brief Prepares a frame for presentation.
     *
     * Consumes scene state, rebuilds draw commands and batches, and writes
     * GPU data. Returns an opaque Frame handle to pass to present().
     *
     * @param desc Describes which views to render. Empty = all registered views.
     * @return Opaque frame handle. Must be passed to present().
     */
    virtual Frame prepare(const FrameDesc& desc = {}) = 0;

    /**
     * @brief Submits a prepared frame to the GPU.
     *
     * Can be called from any thread. All older unpresented frames are
     * silently discarded (their slots recycled).
     *
     * @param frame The frame handle returned by prepare().
     */
    virtual void present(Frame frame) = 0;

    /**
     * @brief Synchronous render of all registered surfaces and cameras.
     *
     * Equivalent to present(prepare()). Prepares draw commands for every
     * registered view, then submits them to the GPU and blocks until
     * the previous frame has been presented (vsync).
     *
     * For asynchronous or selective rendering, use prepare() and present()
     * separately.
     */
    virtual void render() = 0;

    /**
     * @brief Sets the maximum number of frames that can be prepared ahead of presentation.
     *
     * prepare() blocks when this many frames are already waiting. Default is 3.
     * Must be at least 1.
     *
     * @param count Maximum frames in flight.
     */
    virtual void set_max_frames_in_flight(uint32_t count) = 0;

    /** @brief Releases all GPU resources. */
    virtual void shutdown() = 0;
};

} // namespace velk::ui

#endif // VELK_UI_INTF_RENDERER_H
