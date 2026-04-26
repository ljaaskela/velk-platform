#ifndef VELK_UI_INTF_RENDER_TO_TEXTURE_H
#define VELK_UI_INTF_RENDER_TO_TEXTURE_H

#include <velk/api/math_types.h>
#include <velk/interface/intf_metadata.h>

#include <velk-scene/interface/intf_trait.h>

namespace velk {

/**
 * @brief Trait interface for rendering an element's subtree into a texture.
 *
 * When attached to an element, the renderer captures the element's visual
 * subtree into a RenderTexture. The texture can be displayed elsewhere
 * via a TextureVisual bound to the render_target ObjectRef.
 *
 * Set texture_size to a specific resolution, or leave at (0,0) to match
 * the element's layout size.
 */
class IRenderToTexture : public Interface<IRenderToTexture, ITrait>
{
public:
    VELK_INTERFACE(
        (PROP, uvec2, texture_size, {}),           ///< Texture dimensions. (0,0) = match element size.
        (PROP, ObjectRef, render_target, {})        ///< The RenderTexture. Set externally or created by the trait.
    )
};

} // namespace velk

#endif // VELK_UI_INTF_RENDER_TO_TEXTURE_H
