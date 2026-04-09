#ifndef VELK_UI_INTF_ENVIRONMENT_H
#define VELK_UI_INTF_ENVIRONMENT_H

#include <velk/interface/intf_metadata.h>
#include <velk/interface/resource/intf_resource.h>
#include <velk-render/interface/intf_material.h>

namespace velk::ui {

/**
 * @brief An equirectangular HDR environment map for skybox rendering.
 *
 * Loaded via the `env:` resource decoder (e.g. `env:app://hdri/sky.hdr`).
 * The concrete Environment class also implements `ITexture`, so the
 * renderer can bind it as a GPU texture for sampling in the skybox
 * shader. The `IResource` base provides URI, existence, and persistence.
 *
 * Properties:
 *  - `intensity`: exposure multiplier applied to the sampled color.
 *  - `rotation`: Y-axis rotation in degrees, allowing the skybox to be
 *    oriented relative to the scene.
 *
 * Bound to a camera via an `ObjectRef` property on `ICamera`. Multiple
 * cameras can share one environment, or each can have its own.
 */
class IEnvironment : public Interface<IEnvironment, IResource>
{
public:
    VELK_INTERFACE(
        (PROP, float, intensity, 1.f),  ///< Exposure multiplier applied to sampled color.
        (PROP, float, rotation, 0.f)    ///< Y-axis rotation in degrees.
    )

    /**
     * @brief Returns the rendering material for this environment.
     *
     * The Environment owns one IMaterial instance that carries the skybox
     * shader and writes intensity + rotation as per-draw GPU data. The
     * inverse view-projection (needed for ray direction reconstruction)
     * comes from FrameGlobals, so the material is view-independent and
     * can be shared across cameras pointing at the same environment.
     */
    virtual IMaterial::Ptr get_material() const = 0;
};

} // namespace velk::ui

#endif // VELK_UI_INTF_ENVIRONMENT_H
