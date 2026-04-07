#ifndef VELK_UI_IMAGE_API_IMAGE_H
#define VELK_UI_IMAGE_API_IMAGE_H

#include <velk/api/object.h>
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
 * Hides the `instance().resource_store().get_resource<IImage>(uri)` call
 * and provides accessors for status, dimensions, and persistence.
 *
 *   auto img = Image::load("image:app://images/logo.png");
 *   if (img && img.is_loaded()) {
 *       img.set_persistent(true);
 *   }
 *
 *   // Bind into a material:
 *   mat.set_texture(img.as_texture());
 */
class Image : public Object
{
public:
    Image() = default;
    explicit Image(IObject::Ptr obj) : Object(check_object<IImage>(obj)) {}
    explicit Image(IImage::Ptr ptr) : Object(as_object(ptr)) {}

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

    /** @brief Returns this image as an ITexture::Ptr (the same object). */
    ITexture::Ptr as_texture() const { return as_ptr<ITexture>(); }

    /** @brief Returns the image's load status. */
    ImageStatus status() const
    {
        return with_or<IImage>([](auto& i) { return i.status(); }, ImageStatus::Failed);
    }

    /** @brief True if status() == Loaded. */
    bool is_loaded() const { return status() == ImageStatus::Loaded; }

    /** @brief True if status() == Failed. */
    bool is_failed() const { return status() == ImageStatus::Failed; }

    /** @brief Returns the URI this image was loaded from, or empty. */
    string_view uri() const
    {
        return with<IImage>([](auto& i) { return i.uri(); });
    }

    /** @brief Returns true if the image is pinned in the resource store cache. */
    bool is_persistent() const
    {
        return with<IImage>([](auto& i) { return i.is_persistent(); });
    }

    /**
     * @brief Pins or unpins the image in the cache.
     *
     * Persistent images survive even when no consumer holds a reference;
     * the resource store keeps a strong ref. Takes effect on the next
     * resource store access.
     */
    void set_persistent(bool value)
    {
        with<IImage>([&](auto& i) { return i.set_persistent(value); });
    }
};

namespace image {

/**
 *  @brief Loads (or fetches from cache) an image by URI. Equivalent to `Image::load`.
 *  @param uri Uri of the image.
 *  @param persistent If true, the image will not be destroyed when the last reference to it dies.
 */
inline Image load_image(string_view uri, bool persistent = false)
{
    return Image::load(uri);
}

} // namespace image

} // namespace velk::ui

#endif // VELK_UI_IMAGE_API_IMAGE_H
