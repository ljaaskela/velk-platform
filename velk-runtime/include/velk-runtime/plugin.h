#ifndef VELK_RUNTIME_PLUGIN_H
#define VELK_RUNTIME_PLUGIN_H

#include <velk/common.h>

namespace velk {

namespace ClassId {

/** @brief Application runtime. Owns the renderer, manages windows, drives the frame loop. */
inline constexpr Uid Application{"e391d4e6-3b28-4a3a-89f6-91330449606b"};

} // namespace ClassId

namespace PluginId {

/** @brief The velk-runtime core plugin. Registers the Application implementation. */
inline constexpr Uid RuntimePlugin{"25a0cd31-1b2a-4eda-b04d-9bcdc40bf9fe"};

/** @brief Desktop platform plugin (GLFW-backed window provider). */
inline constexpr Uid RuntimeGlfwPlugin{"b2cd54b3-2d75-4c9d-a262-02a2a245c7a8"};

/** @brief Android platform plugin (planned). Placeholder UID until implementation lands. */
inline constexpr Uid RuntimeAndroidPlugin{"00000000-0000-0000-0000-000000000000"};

} // namespace PluginId

} // namespace velk

#endif // VELK_RUNTIME_PLUGIN_H
