#ifndef VELK_UI_API_CAMERA_H
#define VELK_UI_API_CAMERA_H

#include <velk/api/state.h>

#include <velk-render/interface/intf_camera.h>
#include <velk-scene/api/render_trait.h>
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

    auto get_render_path() const { return read_state_value<ICamera>(&ICamera::State::render_path); }
    void set_render_path(RenderPath v) { write_state_value<ICamera>(&ICamera::State::render_path, v); }

    auto get_zoom() const { return read_state_value<ICamera>(&ICamera::State::zoom); }
    void set_zoom(float v) { write_state_value<ICamera>(&ICamera::State::zoom, v); }

    auto get_scale() const { return read_state_value<ICamera>(&ICamera::State::scale); }
    void set_scale(float v) { write_state_value<ICamera>(&ICamera::State::scale, v); }

    auto get_fov() const { return read_state_value<ICamera>(&ICamera::State::fov); }
    void set_fov(float v) { write_state_value<ICamera>(&ICamera::State::fov, v); }
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
