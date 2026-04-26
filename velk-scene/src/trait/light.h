#ifndef VELK_UI_TRAIT_LIGHT_H
#define VELK_UI_TRAIT_LIGHT_H

#include <velk/ext/object.h>

#include <velk-render/interface/intf_light.h>
#include <velk-scene/plugin.h>

namespace velk::impl {

/**
 * @brief Default light render-trait implementation.
 *
 * Pure data container — properties via ILight's state macro. The
 * renderer reads the owning element's world_matrix for position and
 * forward axis; this class holds only the intrinsic source properties.
 */
class Light : public ::velk::ext::Object<Light, ::velk::ILight>
{
public:
    VELK_CLASS_UID(ClassId::Render::Light, "Light");
};

} // namespace velk::impl

#endif // VELK_UI_TRAIT_LIGHT_H
