#ifndef VELK_UI_ENV_API_ENVIRONMENT_H
#define VELK_UI_ENV_API_ENVIRONMENT_H

#include <velk/api/resource.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/interface/resource/intf_resource_store.h>

#include <velk-ui/interface/intf_environment.h>
#include <velk-ui/plugins/image/plugin.h>

namespace velk::ui {

/**
 * @brief Convenience wrapper around IEnvironment.
 *
 * Inherits Resource for URI, existence, size, and persistence accessors.
 *
 * @code
 *   auto env = load_environment("env:app://hdri/sky.hdr");
 *   env.set_intensity(1.5f);
 *   env.set_rotation(45.f);
 * @endcode
 */
class Environment : public Resource
{
public:
    /** @brief Default-constructed Environment wraps no object. */
    Environment() = default;

    /** @brief Wraps an existing IObject pointer, rejected if it does not implement IEnvironment. */
    explicit Environment(IObject::Ptr obj) : Resource(check_object<IEnvironment>(obj)) {}

    /** @brief Wraps an existing IEnvironment pointer. */
    explicit Environment(IEnvironment::Ptr e) : Resource(as_object(e)) {}

    /** @brief Implicit conversion to IEnvironment::Ptr. */
    operator IEnvironment::Ptr() const { return as_ptr<IEnvironment>(); }

    /** @brief Sets the exposure multiplier. */
    void set_intensity(float v)
    {
        write_state_value<IEnvironment>(&IEnvironment::State::intensity, v);
    }

    /** @brief Returns the exposure multiplier. */
    auto get_intensity() const
    {
        return read_state_value<IEnvironment>(&IEnvironment::State::intensity);
    }

    /** @brief Sets the Y-axis rotation in degrees. */
    void set_rotation(float deg)
    {
        write_state_value<IEnvironment>(&IEnvironment::State::rotation, deg);
    }

    /** @brief Returns the Y-axis rotation in degrees. */
    auto get_rotation() const
    {
        return read_state_value<IEnvironment>(&IEnvironment::State::rotation);
    }
};

/**
 * @brief Loads an environment map from a URI.
 *
 * @param uri Resource URI, e.g. `"env:app://hdri/sky.hdr"`. The `env:`
 *            prefix routes through the environment decoder which loads
 *            the HDR image via stb_image and stores it as RGBA16F.
 * @return A fully decoded Environment ready for binding to a camera via
 *         ObjectRef. The resource store caches the result, so subsequent
 *         calls with the same URI return the same object.
 *
 * @code
 *   auto env = load_environment("env:app://hdri/sky.hdr");
 *   write_state<ICamera>(camera, [&](auto& s) {
 *       set_object_ref(s.environment, env);
 *   });
 * @endcode
 */
inline Environment load_environment(string_view uri)
{
    auto& store = instance().resource_store();
    return Environment(store.get_resource<IEnvironment>(uri));
}

} // namespace velk::ui

#endif // VELK_UI_ENV_API_ENVIRONMENT_H
