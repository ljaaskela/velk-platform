#ifndef VELK_UI_IMAGE_IMPL_H
#define VELK_UI_IMAGE_IMPL_H

#include <velk/ext/object.h>
#include <velk/string.h>
#include <velk/vector.h>

#include <velk-render/ext/gpu_resource_mixin.h>
#include <velk-render/interface/intf_image.h>
#include <velk-render/interface/intf_texture.h>
#include <velk-ui/plugins/image/plugin.h>

#include <cstdint>

namespace velk::ui::impl {

/**
 * @brief Decoded raster image. Owns CPU pixels until the renderer uploads
 *        them, after which the pixel buffer is freed.
 *
 * Implements both `IImage` (the URI-resource view used by the resource
 * store and apps) and `ITexture` (the GPU binding view used by materials
 * and the renderer). The same object serves both roles.
 */
class Image final : public ::velk::ext::Object<Image, IImage, ITexture>,
                    public ::velk::ext::GpuResourceMixin
{
public:
    VELK_CLASS_UID(::velk::ui::ClassId::Image, "Image");

    Image();
    ~Image() override;

    /// Initializes from decoded RGBA8 pixels. Sets status to Loaded on
    /// success, Failed otherwise.
    void init(string_view uri, int width, int height, PixelFormat format,
              vector<uint8_t> pixels);

    /// Constructs a failed image with no pixel data, just a uri and Failed status.
    void init_failed(string_view uri);

    void set_persistent_flag(bool value) { persistent_ = value; }

    // IResource
    string_view uri() const override { return uri_; }
    bool exists() const override { return status_ == ImageStatus::Loaded; }
    int64_t size() const override { return static_cast<int64_t>(pixels_.size()); }
    bool is_persistent() const override { return persistent_; }
    void set_persistent(bool value) override { persistent_ = value; }

    // IImage
    ImageStatus status() const override { return status_; }

    // ITexture
    int width() const override { return width_; }
    int height() const override { return height_; }
    PixelFormat format() const override { return format_; }
    const uint8_t* get_pixels() const override
    {
        return pixels_.empty() ? nullptr : pixels_.data();
    }
    bool is_dirty() const override { return dirty_; }
    void clear_dirty() override
    {
        dirty_ = false;
        // Free CPU pixels: they have been uploaded to the GPU and will not
        // be re-uploaded (static images do not become dirty again).
        pixels_ = vector<uint8_t>{};
    }

    // IGpuResource
    void add_gpu_resource_observer(IGpuResourceObserver* obs) override
    {
        GpuResourceMixin::add_observer(obs);
    }
    void remove_gpu_resource_observer(IGpuResourceObserver* obs) override
    {
        GpuResourceMixin::remove_observer(obs);
    }

private:
    string uri_;
    int width_{};
    int height_{};
    PixelFormat format_{PixelFormat::RGBA8_SRGB};
    vector<uint8_t> pixels_;
    ImageStatus status_{ImageStatus::Unloaded};
    bool dirty_{false};
    bool persistent_{false};
};

} // namespace velk::ui::impl

#endif // VELK_UI_IMAGE_IMPL_H
