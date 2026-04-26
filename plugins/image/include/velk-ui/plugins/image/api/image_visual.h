#ifndef VELK_UI_IMAGE_API_IMAGE_VISUAL_H
#define VELK_UI_IMAGE_API_IMAGE_VISUAL_H

#include <velk/api/state.h>

#include <velk-scene/api/visual/visual.h>
#include <velk-ui/plugins/image/intf_image_visual.h>
#include <velk-ui/plugins/image/plugin.h>

namespace velk::ui {

/**
 * @brief Convenience wrapper around an ImageVisual.
 *
 * Inherits color and paint accessors from Visual. The visual loads the
 * image lazily on uri change via the resource store and binds it to an
 * internal ImageMaterial.
 *
 *   auto iv = trait::visual::create_image();
 *   iv.set_uri("image:app://images/logo.png");
 *   iv.set_tint(color::white());
 *   element.add_attachment(iv);
 */
class ImageVisual : public Visual2D
{
public:
    ImageVisual() = default;

    /** @brief Wraps an existing IObject pointer, rejected if it does not implement IImageVisual. */
    explicit ImageVisual(IObject::Ptr obj) : Visual2D(check_object<IImageVisual>(obj)) {}

    /** @brief Wraps an existing IImageVisual pointer. */
    explicit ImageVisual(IImageVisual::Ptr iv) : Visual2D(as_object(iv)) {}

    /** @brief Returns the URI of the image to load. */
    auto get_uri() const { return read_state_value<IImageVisual>(&IImageVisual::State::uri); }

    /** @brief Sets the URI of the image to load (e.g. "image:app://logo.png"). */
    void set_uri(string_view uri)
    {
        write_state_value<IImageVisual>(&IImageVisual::State::uri, string(uri));
    }

    /** @brief Returns the multiplicative tint applied to the sampled texture. */
    auto get_tint() const { return read_state_value<IImageVisual>(&IImageVisual::State::tint); }

    /** @brief Sets the multiplicative tint (default: white). */
    void set_tint(const color& v) { write_state_value<IImageVisual>(&IImageVisual::State::tint, v); }
};

namespace trait::visual {

/** @brief Creates a new ImageVisual. */
inline ImageVisual create_image()
{
    return ImageVisual(instance().create<IObject>(ClassId::Visual::Image));
}

/** @brief Creates a new ImageVisual loading @p uri. */
inline ImageVisual create_image(string_view uri)
{
    auto v = create_image();
    v.set_uri(uri);
    return v;
}

} // namespace trait::visual

} // namespace velk::ui

#endif // VELK_UI_IMAGE_API_IMAGE_VISUAL_H
