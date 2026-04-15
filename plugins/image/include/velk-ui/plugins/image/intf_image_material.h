#ifndef VELK_UI_INTF_IMAGE_MATERIAL_H
#define VELK_UI_INTF_IMAGE_MATERIAL_H

#include <velk/api/math_types.h>
#include <velk/api/object_ref.h>
#include <velk/interface/intf_metadata.h>

namespace velk::ui {

/**
 * @brief Properties for the image material.
 *
 * `texture` is an ObjectRef. Any object that implements `ISurface` works:
 * a decoded `Image` (which implements both `IImage` and `ISurface`),
 * a render target texture, etc.
 *
 * `tint` is multiplied with the sampled texel. Defaults to white (no tint).
 */
class IImageMaterial : public Interface<IImageMaterial>
{
public:
    VELK_INTERFACE(
        (PROP, ObjectRef, texture, {}),
        (PROP, color, tint, (color::white()))
    )
};

} // namespace velk::ui

#endif // VELK_UI_INTF_IMAGE_MATERIAL_H
