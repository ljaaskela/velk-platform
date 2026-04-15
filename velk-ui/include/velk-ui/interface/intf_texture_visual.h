#ifndef VELK_UI_INTF_TEXTURE_VISUAL_H
#define VELK_UI_INTF_TEXTURE_VISUAL_H

#include <velk/interface/intf_interface.h>

namespace velk::ui {

/**
 * @brief Visual trait that displays a texture on the element bounds.
 *
 * The texture is referenced via ObjectRef, allowing binding to any ISurface
 * including RenderTexture outputs. The tint color multiplies the sampled texel.
 */
class ITextureVisual : public Interface<ITextureVisual>
{
public:
    VELK_INTERFACE(
        (PROP, ObjectRef, texture, {}),                  ///< Reference to an ISurface to display.
        (PROP, ::velk::color, tint, (::velk::color::white())) ///< Tint color multiplied with sampled texel.
    )
};

} // namespace velk::ui

#endif // VELK_UI_INTF_TEXTURE_VISUAL_H
