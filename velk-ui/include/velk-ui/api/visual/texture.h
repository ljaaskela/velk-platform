#ifndef VELK_UI_API_VISUAL_TEXTURE_H
#define VELK_UI_API_VISUAL_TEXTURE_H

#include <velk/api/object_ref.h>
#include <velk/api/state.h>
#include <velk-ui/api/visual/visual.h>
#include <velk-ui/interface/intf_texture_visual.h>
#include <velk-ui/plugin.h>

namespace velk::ui {

/**
 * @brief Convenience wrapper around a TextureVisual.
 *
 * Displays a texture on the element bounds. The texture is set via ObjectRef,
 * allowing it to reference any ISurface including RenderTexture outputs.
 *
 *   auto tv = trait::visual::create_texture_visual();
 *   tv.set_texture(my_texture);
 */
class TextureVisual : public Visual
{
public:
    TextureVisual() = default;
    explicit TextureVisual(IObject::Ptr obj) : Visual(std::move(obj)) {}
    explicit TextureVisual(IVisual::Ptr v) : Visual(as_object(v)) {}

    /** @brief Sets the texture to display via ObjectRef. */
    void set_texture(const IObject::Ptr& texture)
    {
        write_state<ITextureVisual>(ptr(), [&](ITextureVisual::State& s) {
            set_object_ref(s.texture, texture);
        });
    }

    /** @brief Sets the tint color. */
    void set_tint(const color& c)
    {
        write_state<ITextureVisual>(ptr(), [&](ITextureVisual::State& s) {
            s.tint = c;
        });
    }
};

namespace trait::visual {

/** @brief Creates a new TextureVisual. */
inline TextureVisual create_texture_visual()
{
    return TextureVisual(instance().create<IObject>(ClassId::Visual::Texture));
}

} // namespace trait::visual

} // namespace velk::ui

#endif // VELK_UI_API_VISUAL_TEXTURE_H
