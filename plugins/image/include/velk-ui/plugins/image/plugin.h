#ifndef VELK_UI_IMAGE_PLUGIN_H
#define VELK_UI_IMAGE_PLUGIN_H

#include <velk/common.h>

namespace velk::ui {

namespace ClassId {

/** @brief Decoded raster image; implements IImage, ISurface, and IBuffer. */
inline constexpr Uid Image{"1933a69f-eb6e-438d-becb-2d9923ee84a6"};

/** @brief Decoder turning raw image bytes into Image objects. Registered as "image". */
inline constexpr Uid ImageDecoder{"d595679f-ed96-4af3-b208-83eb3e2d26b3"};

/** @brief Equirectangular HDR environment map; implements IEnvironment, ISurface, and IBuffer. */
inline constexpr Uid Environment{"c8531002-51c3-4b7c-abb7-5d89586d6696"};

/** @brief Decoder turning raw HDR bytes into Environment objects. Registered as "env". */
inline constexpr Uid EnvDecoder{"6d60e405-6d91-478c-89d1-47a51f463236"};

namespace Visual {

/** @brief Convenience visual that loads an image by URI and binds it as a textured quad. */
inline constexpr Uid Image{"e72f6424-bd52-4ceb-9f27-2ae95a32297b"};

} // namespace Visual

namespace Material {

/** @brief Material sampling an ISurface and multiplying by a tint. */
inline constexpr Uid Image{"95b06fa1-fc1a-4e26-adb0-2eecdd39c641"};

/** @brief Material rendering an equirectangular environment as a skybox. */
inline constexpr Uid Environment{"1bdcf257-b2a6-4d26-a32f-ab0eb7903646"};

} // namespace Material

} // namespace ClassId

namespace PluginId {

inline constexpr Uid ImagePlugin{"87a005fe-c9c9-4012-a6b8-1d41f3029861"};

} // namespace PluginId

} // namespace velk::ui

#endif // VELK_UI_IMAGE_PLUGIN_H
