#ifndef VELK_UI_TEXTURE_VISUAL_H
#define VELK_UI_TEXTURE_VISUAL_H

#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_surface.h>
#include <velk-scene/ext/trait.h>
#include <velk-ui/interface/intf_texture_visual.h>
#include <velk-ui/plugin.h>

namespace velk::ui::impl {

/**
 * @brief Visual trait that displays a texture on the element bounds.
 *
 * References a texture via ObjectRef. The texture can be any ISurface,
 * including a RenderTexture from a RenderToTexture trait.
 */
class TextureVisual : public ::velk::ext::Visual2D<TextureVisual, ITextureVisual>
{
public:
    VELK_CLASS_UID(::velk::ui::ClassId::Visual::Texture, "TextureVisual");

    vector<DrawEntry> get_draw_entries(::velk::IRenderContext& ctx,
                                       const ::velk::size& bounds) override;
    vector<IBuffer::Ptr> get_gpu_resources(::velk::IRenderContext& ctx) const override;

protected:
    void on_state_changed(string_view name, IMetadata& owner, Uid interfaceId) override;

private:
    void ensure_material();

    IObject::Ptr material_;
};

} // namespace velk::ui::impl

#endif // VELK_UI_TEXTURE_VISUAL_H
