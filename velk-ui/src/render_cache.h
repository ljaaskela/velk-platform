#ifndef VELK_UI_RENDER_CACHE_H
#define VELK_UI_RENDER_CACHE_H

#include <velk-scene/ext/trait.h>
#include <velk-scene/interface/intf_render_to_texture.h>
#include <velk-ui/plugin.h>

namespace velk::ui::impl {

/**
 * @brief Caches an element's rendered subtree into a RenderTexture.
 *
 * When attached to an element, the renderer captures the subtree's
 * visual output into a texture. The texture can be displayed elsewhere
 * via a TextureVisual bound to the render_target ObjectRef.
 */
class RenderCache : public ::velk::ext::Render<RenderCache, IRenderToTexture>
{
public:
    VELK_CLASS_UID(::velk::ui::ClassId::Render::RenderCache, "RenderCache");

    RenderCache() = default;

protected:
    void on_state_changed(string_view name, IMetadata& owner, Uid interfaceId) override;
};

} // namespace velk::ui::impl

#endif // VELK_UI_RENDER_CACHE_H
