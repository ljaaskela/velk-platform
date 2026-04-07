#ifndef VELK_UI_IMAGE_API_IMAGE_MATERIAL_H
#define VELK_UI_IMAGE_API_IMAGE_MATERIAL_H

#include <velk/api/object.h>
#include <velk/api/object_ref.h>
#include <velk/api/state.h>

#include <velk-render/interface/intf_image.h>
#include <velk-render/interface/intf_material.h>
#include <velk-render/interface/intf_texture.h>
#include <velk-ui/plugins/image/intf_image_material.h>
#include <velk-ui/plugins/image/plugin.h>

namespace velk::ui {

/**
 * @brief Convenience wrapper around an ImageMaterial.
 *
 * Provides null-safe access to the texture and tint properties.
 *
 *   auto img = velk::instance().resource_store().get_resource<IImage>(
 *       "image:app://images/logo.png");
 *   auto mat = material::create_image();
 *   mat.set_texture(img);
 *   mat.set_tint(color::white());
 *   rect_visual.set_paint(mat);
 *
 * The texture property is `ObjectRef<ITexture>`, so any object that
 * implements `ITexture` works: a decoded `Image` (which implements both
 * `IImage` and `ITexture`), a glyph atlas, a future render target.
 */
class ImageMaterial : public Object
{
public:
    ImageMaterial() = default;
    explicit ImageMaterial(IObject::Ptr obj) : Object(check_object<IImageMaterial>(obj)) {}
    explicit ImageMaterial(IImageMaterial::Ptr im) : Object(as_object(im)) {}
    operator IMaterial::Ptr() const { return as_ptr<IMaterial>(); }

    /** @brief Sets the bound texture. */
    void set_texture(const ITexture::Ptr& tex)
    {
        write_state<IImageMaterial>([&](IImageMaterial::State& s) {
            set_object_ref(s.texture, tex);
        });
    }

    /** @brief Convenience: bind an IImage (which also implements ITexture). */
    void set_texture(const IImage::Ptr& img)
    {
        set_texture(interface_pointer_cast<ITexture>(img));
    }

    /** @brief Returns the bound texture, or nullptr. */
    ITexture::Ptr get_texture() const
    {
        if (auto state = read_state<IImageMaterial>()) {
            return state->texture.template get<ITexture>();
        }
        return nullptr;
    }

    /** @brief Returns the multiplicative tint. */
    auto get_tint() const { return read_state_value<IImageMaterial>(&IImageMaterial::State::tint); }

    /** @brief Sets the multiplicative tint (default: white). */
    void set_tint(const color& v) { write_state_value<IImageMaterial>(&IImageMaterial::State::tint, v); }
};

namespace material {

/** @brief Creates a new ImageMaterial. */
inline ImageMaterial create_image()
{
    return ImageMaterial(instance().create<IObject>(ClassId::Material::Image));
}

/** @brief Creates a new ImageMaterial bound to @p texture. */
inline ImageMaterial create_image(const ITexture::Ptr& texture)
{
    auto m = create_image();
    m.set_texture(texture);
    return m;
}

/** @brief Creates a new ImageMaterial bound to @p image. */
inline ImageMaterial create_image(const IImage::Ptr& image)
{
    auto m = create_image();
    m.set_texture(image);
    return m;
}

} // namespace material

} // namespace velk::ui

#endif // VELK_UI_IMAGE_API_IMAGE_MATERIAL_H
