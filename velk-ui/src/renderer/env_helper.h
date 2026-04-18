#ifndef VELK_UI_ENV_HELPER_H
#define VELK_UI_ENV_HELPER_H

#include <velk-render/interface/intf_camera.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-ui/interface/intf_environment.h>

#include "view_renderer.h"

namespace velk::ui {

/**
 * @brief Result of ensuring a camera's environment is GPU-ready.
 *
 * All fields are zero / null when the camera has no environment or the
 * environment is not yet loaded. Callers must null-check env.
 */
struct EnvResolved
{
    IEnvironment* env = nullptr;     ///< Raw pointer into the camera's env object (non-owning; lifetime managed by the scene).
    ISurface* surface = nullptr;     ///< The environment's bindless surface.
    TextureId texture_id = 0;        ///< Bindless index of the uploaded texture.
};

/**
 * @brief Ensures the given camera's environment texture is uploaded and
 *        registered in the bindless pool.
 *
 * Handles the one-time create_texture + register_texture + observer wiring
 * and the per-frame upload when IBuffer::is_dirty() is set. Idempotent; safe
 * to call from multiple sub-renderers per frame per camera.
 *
 * Returns an EnvResolved describing what was found. If the camera has no
 * environment, returns an all-zero struct.
 */
EnvResolved ensure_env_ready(ICamera& camera, FrameContext& ctx);

} // namespace velk::ui

#endif // VELK_UI_ENV_HELPER_H
