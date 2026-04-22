#ifndef VELK_RENDER_INTF_RENDER_TARGET_H
#define VELK_RENDER_INTF_RENDER_TARGET_H

#include <velk-render/interface/intf_surface.h>

namespace velk {

/**
 * @brief A surface that can be rendered into.
 *
 * Both swapchain windows and renderable textures implement this interface.
 * The render target id is the backend handle passed to begin_pass().
 *
 * Chain: IInterface -> IGpuResource -> ISurface -> IRenderTarget
 */
class IRenderTarget : public Interface<IRenderTarget, ISurface>
{
public:
    /**
     * @brief Returns the backend handle for this render target.
     *
     * For window surfaces, this is the surface_id from create_surface().
     * For renderable textures, this is the TextureId (bindless index).
     * Passed to the backend's begin_pass() to select the target.
     */
    virtual uint64_t get_render_target_id() const = 0;

    /** @brief Sets the backend handle. Called by the renderer after creating the backend resource. */
    virtual void set_render_target_id(uint64_t id) = 0;

    /**
     * @brief Returns the depth attachment format of this target.
     *
     * DepthFormat::None means the target has no depth attachment. The backend
     * uses this to decide whether to wire depth state on pipelines and to
     * clear depth at pass begin.
     */
    virtual DepthFormat get_depth_format() const { return DepthFormat::None; }

    /** @brief Sets the depth attachment format. Called during target creation. */
    virtual void set_depth_format(DepthFormat /*df*/) {}
};

/**
 * @brief Returns the render target id from any interface pointer that supports IRenderTarget.
 */
template <typename T>
uint64_t get_render_target_id(const T& ptr)
{
    auto* rt = interface_cast<IRenderTarget>(ptr);
    return rt ? rt->get_render_target_id() : 0;
}

} // namespace velk

#endif // VELK_RENDER_INTF_RENDER_TARGET_H
