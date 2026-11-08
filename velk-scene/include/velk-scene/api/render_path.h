#ifndef VELK_SCENE_API_RENDER_PATH_H
#define VELK_SCENE_API_RENDER_PATH_H

#include <velk/api/object.h>

#include <velk-scene/plugin.h>
#include <velk-render/interface/intf_render_path.h>

namespace velk {

/**
 * @brief Typed wrapper around an IRenderPath object.
 *
 * Render paths attach to a Camera trait via `Camera::add_render_path`.
 * Concrete path wrappers (`ForwardPath`, `DeferredPath`, `RtPath`)
 * derive from this so user code stays terse:
 *
 *   camera.add_render_path(path::create_deferred());
 */
class RenderPath : public Object
{
public:
    RenderPath() = default;

    explicit RenderPath(IObject::Ptr obj)
        : Object(check_object<IRenderPath>(obj)) {}

    explicit RenderPath(IRenderPath::Ptr p)
        : Object(as_object(p)) {}

    operator IRenderPath::Ptr() const { return as_ptr<IRenderPath>(); }
};

namespace path {

/** @brief Forward shading. Default fallback if no path is attached. */
inline RenderPath create_forward()
{
    return RenderPath(instance().create<IObject>(ClassId::Path::Forward));
}

/** @brief Deferred shading: G-buffer fill + compute lighting + blit. */
inline RenderPath create_deferred()
{
    return RenderPath(instance().create<IObject>(ClassId::Path::Deferred));
}

/** @brief Compute-shader path tracer. */
inline RenderPath create_rt()
{
    return RenderPath(instance().create<IObject>(ClassId::Path::Rt));
}

/** @brief Path tracing accumulated over frames, for reference images. */
inline RenderPath create_reference()
{
    return RenderPath(instance().create<IObject>(ClassId::Path::Reference));
}

} // namespace path

} // namespace velk

#endif // VELK_SCENE_API_RENDER_PATH_H
