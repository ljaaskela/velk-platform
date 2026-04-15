#ifndef VELK_UI_IMAGE_API_IMAGE_H
#define VELK_UI_IMAGE_API_IMAGE_H

#include <velk/api/resource.h>
#include <velk/api/velk.h>
#include <velk/string_view.h>

#include <velk-render/interface/intf_image.h>
#include <velk-render/interface/intf_texture.h>
#include <velk-ui/plugins/image/plugin.h>

namespace velk::ui {

/**
 * @brief Convenience wrapper around an IImage produced by the image
 *        decoder and cached in the resource store.
 *
 * Inherits Resource for URI, existence, size, and persistence accessors.
 *
 * @code
 *   auto img = Image::load("image:app://images/logo.png");
 *   if (img && img.is_loaded()) {
 *       img.set_persistent(true);
 *   }
 *   mat.set_texture(img.as_texture());
 * @endcode
 */
class Image : public Resource
{
public:
    /** @brief Default-constructed Image wraps no object. */
    Image() = default;

    /** @brief Wraps an existing IObject pointer, rejected if it does not implement IImage. */
    explicit Image(IObject::Ptr obj) : Resource(check_object<IImage>(obj)) {}

    /** @brief Wraps an existing IImage pointer. */
    explicit Image(IImage::Ptr ptr) : Resource(as_object(ptr)) {}

    /**
     * @brief Loads (or fetches from cache) an image by URI.
     *
     * The URI must be a decoder URI of the form `image:<inner_uri>`,
     * e.g. `image:app://images/logo.png`. Subsequent calls with the same
     * URI return the same underlying `IImage` while at least one consumer
     * holds a reference.
     *
     * @return An `Image` wrapping the result. The wrapper may evaluate
     *         to false if the URI's protocol scheme is unknown or the
     *         decoder rejected the input outright; check `is_loaded()`
     *         to distinguish a successful load from a cached failure.
     */
    static Image load(string_view uri)
    {
        auto& store = ::velk::instance().resource_store();
        return Image(store.get_resource<IImage>(uri));
    }

    /** @brief Implicit conversion to IImage::Ptr. */
    operator IImage::Ptr() const { return as_ptr<IImage>(); }

    /** @brief Returns this image as an ISurface::Ptr (the same object). */
    ISurface::Ptr as_surface() const { return as_ptr<ISurface>(); }

    /** @brief Returns the image's load status. */
    ImageStatus status() const
    {
        return with_or<IImage>([](auto& i) { return i.status(); }, ImageStatus::Failed);
    }

    /** @brief True if status() == Loaded. */
    bool is_loaded() const { return status() == ImageStatus::Loaded; }

    /** @brief True if status() == Failed. */
    bool is_failed() const { return status() == ImageStatus::Failed; }
};

namespace image {

/**
 * @brief Loads (or fetches from cache) an image by URI. Equivalent to `Image::load`.
 * @param uri URI of the image.
 * @param persistent If true, the image will not be destroyed when the last reference to it dies.
 */
inline Image load_image(string_view uri, bool persistent = false)
{
    auto img = Image::load(uri);
    if (persistent && img) {
        img.set_persistent(true);
    }
    return img;
}

} // namespace image

} // namespace velk::ui

#endif // VELK_UI_IMAGE_API_IMAGE_H
