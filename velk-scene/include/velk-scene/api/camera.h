#ifndef VELK_UI_API_CAMERA_H
#define VELK_UI_API_CAMERA_H

#include <velk/api/object_ref.h>
#include <velk/api/state.h>

#include <velk-render/interface/intf_camera.h>
#include <velk-render/interface/intf_view_pipeline.h>
#include <velk-scene/api/post_process.h>
#include <velk-scene/api/render_path.h>
#include <velk-scene/api/render_trait.h>
#include <velk-scene/interface/intf_environment.h>
#include <velk-scene/plugin.h>

namespace velk {

/**
 * @brief Convenience wrapper around ICamera.
 *
 *   auto cam = create_camera();
 *   element.add_trait(cam);
 *   cam.add_technique(create_rt_reflection());   // render-config attachment
 */
class Camera : public RenderTrait
{
public:
    Camera() = default;
    explicit Camera(IObject::Ptr obj) : RenderTrait(check_object<ICamera>(obj)) {}
    explicit Camera(ICamera::Ptr c) : RenderTrait(as_object(c)) {}

    operator ICamera::Ptr() const { return as_ptr<ICamera>(); }

    auto get_projection() const { return read_state_value<ICamera>(&ICamera::State::projection); }
    void set_projection(Projection v) { write_state_value<ICamera>(&ICamera::State::projection, v); }

    /// Attaches a render path to this camera. Replaces any previous
    /// attachment of the same type.
    ReturnValue add_render_path(const RenderPath& path)
    {
        return path ? add_attachment(static_cast<IRenderPath::Ptr>(path))
                    : ReturnValue::InvalidArgument;
    }

    /// Removes a previously attached render path.
    ReturnValue remove_render_path(const RenderPath& path)
    {
        return path ? remove_attachment(static_cast<IRenderPath::Ptr>(path))
                    : ReturnValue::InvalidArgument;
    }

    /// Attaches a post-process (or chain) to this camera's default
    /// view pipeline. Convenience for the common case; users with
    /// custom IViewPipelines attach directly via the pipeline trait.
    ReturnValue add_post_process(const PostProcess& post)
    {
        if (!post) return ReturnValue::InvalidArgument;
        auto pipeline = find_attachment<IViewPipeline>();
        if (!pipeline) return ReturnValue::Fail;
        return Object(as_object(pipeline)).add_attachment(
            static_cast<IPostProcess::Ptr>(post));
    }

    /// Removes a previously attached post-process from the default pipeline.
    ReturnValue remove_post_process(const PostProcess& post)
    {
        if (!post) return ReturnValue::InvalidArgument;
        auto pipeline = find_attachment<IViewPipeline>();
        if (!pipeline) return ReturnValue::Fail;
        return Object(as_object(pipeline)).remove_attachment(
            static_cast<IPostProcess::Ptr>(post));
    }

    auto get_zoom() const { return read_state_value<ICamera>(&ICamera::State::zoom); }
    void set_zoom(float v) { write_state_value<ICamera>(&ICamera::State::zoom, v); }

    auto get_scale() const { return read_state_value<ICamera>(&ICamera::State::scale); }
    void set_scale(float v) { write_state_value<ICamera>(&ICamera::State::scale, v); }

    auto get_fov() const { return read_state_value<ICamera>(&ICamera::State::fov); }
    void set_fov(float v) { write_state_value<ICamera>(&ICamera::State::fov, v); }

    /// Attaches an environment (equirect HDR) to this camera. The deferred
    /// and RT paths sample it as the ambient / IBL term and draw it as the
    /// background sky. Pass a null Ptr to clear the environment.
    void set_environment(const IEnvironment::Ptr& env)
    {
        write_state<ICamera>([&](ICamera::State& s) { set_object_ref(s.environment, env); });
    }
};

namespace trait::render {

/** @brief Creates a new Camera trait with default orthographic projection. */
inline Camera create_camera()
{
    return Camera(instance().create<ICamera>(ClassId::Render::Camera));
}

} // namespace trait::render

} // namespace velk

#endif // VELK_UI_API_CAMERA_H
