#ifndef VELK_UI_INTF_IMAGE_VISUAL_H
#define VELK_UI_INTF_IMAGE_VISUAL_H

#include <velk/api/math_types.h>
#include <velk/interface/intf_metadata.h>
#include <velk/string.h>

namespace velk::ui {

/**
 * @brief Properties for the image visual.
 *
 * `uri` is fetched lazily on change via the resource store. The result is
 * an `IImage`/`ISurface` that the visual binds to its internal `ImageMaterial`.
 *
 * `tint` is forwarded to the material and multiplies the sampled texel.
 */
class IImageVisual : public Interface<IImageVisual>
{
public:
    VELK_INTERFACE(
        (PROP, string, uri, {}),
        (PROP, color, tint, (color::white()))
    )
};

} // namespace velk::ui

#endif // VELK_UI_INTF_IMAGE_VISUAL_H
