#ifndef VELK_UI_IMAGE_IMPL_H
#define VELK_UI_IMAGE_IMPL_H

#include <velk/ext/object.h>
#include <velk/string.h>
#include <velk/vector.h>

#include <velk-render/ext/gpu_resource.h>
#include <velk-render/interface/intf_image.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_surface.h>
#include <velk-ui/plugins/image/plugin.h>

#include <cstdint>

namespace velk::ui::impl {

/// The mean colours of RGBA8 texels (ISurface::get_average_color), alpha weighted: as linear
/// values, and as sRGB values made linear. Which one an image gives depends on its format.
struct ImageAverages
{
    color raw = color::white();
    color from_srgb = color::white();
    bool known = false;
};

/// Works out the averages of @p pixel_count RGBA8 texels at @p pixels.
ImageAverages average_rgba8(const uint8_t* pixels, size_t pixel_count);

/**
 * @brief Decoded raster image. Owns CPU pixels until the renderer uploads
 *        them, after which the pixel buffer is freed.
 *
 * Implements both `IImage` (the URI-resource view used by the resource
 * store and apps) and `ISurface` + `IBuffer` (the GPU binding view used by materials
 * and the renderer). The same object serves both roles.
 */
class Image final : public ::velk::ext::GpuResource<Image, IImage, ISurface, IBuffer>
{
public:
    VELK_CLASS_UID(::velk::ui::ClassId::Image, "Image");

    GpuResourceType get_type() const override { return GpuResourceType::Texture; }

    Image();
    // Destructor is provided by ext::GpuResource (it notifies observers).

    /// Initializes from decoded RGBA8 pixels. Sets status to Loaded on
    /// success, Failed otherwise.
    void init(string_view uri, int width, int height, PixelFormat format,
              vector<uint8_t> pixels);

    /// Constructs a failed image with no pixel data, just a uri and Failed status.
    void init_failed(string_view uri);

    /// Marks the image as Loading (0x0, no pixels) while a worker decodes it.
    void begin_loading(string_view uri);

    /// Completes an asynchronous load with decoded pixels and their averages
    /// (average_rgba8, worked out where they were decoded). Keeps the format
    /// and sampler set by loaders in the meantime. Fires on_loaded.
    void finish_loading(int width, int height, vector<uint8_t> pixels, const ImageAverages& averages);

    /// Completes an asynchronous load of a file with levels of its own (KTX): @p levels of
    /// @p format in @p data. A colour space a loader set in the meantime (set_format) is kept,
    /// on the file's format. Fires on_loaded.
    void finish_loading_levels(int width, int height, PixelFormat format, uint32_t levels, vector<uint8_t> data,
                               const ImageAverages& averages);

    /// Completes an asynchronous load as Failed. Fires on_loaded.
    void fail_loading();

    void set_persistent_flag(bool value) { persistent_ = value; }

    /// Sets the sampler desc the renderer should use when uploading this
    /// image. Must be called before the image is first observed by the
    /// renderer (subsequent changes won't replace the descriptor).
    void set_sampler_desc(const SamplerDesc& desc) override { sampler_desc_ = desc; }

    /// Overrides the GPU pixel format / color space before upload. Used by
    /// loaders to mark non-color data (normal / MR / occlusion) maps linear.
    /// RGBA8 or RGBA8_SRGB on a compressed image only sets its colour space.
    void set_format(PixelFormat format) override;

    /// IImage interface entry: copy pixels and call init().
    void init_from_pixels(string_view uri, int width, int height, PixelFormat format,
                          const uint8_t* pixels, size_t pixel_size) override;

    // IResource
    string_view uri() const override { return uri_; }
    bool exists() const override { return status_ == ImageStatus::Loaded; }
    int64_t size() const override { return static_cast<int64_t>(pixels_.size()); }
    bool is_persistent() const override { return persistent_; }
    void set_persistent(bool value) override { persistent_ = value; }

    // IImage
    ImageStatus status() const override { return status_; }
    uint32_t get_mip_levels() const override { return mip_levels_; }

    // IBuffer
    size_t get_data_size() const override { return pixels_.size(); }
    const uint8_t* get_data() const override
    {
        return pixels_.empty() ? nullptr : pixels_.data();
    }
    bool is_dirty() const override { return dirty_; }
    bool write_diff(const void* /*bytes*/, size_t /*size*/) override { return false; }
    bool write(size_t /*sz*/, WriteFn /*fn*/, void* /*ctx*/) override { return false; }
    void clear_dirty() override
    {
        dirty_ = false;
        // Free CPU pixels: they have been uploaded to the GPU and will not
        // be re-uploaded (static images do not become dirty again).
        pixels_ = vector<uint8_t>{};
    }

    // ISurface
    uvec2 get_dimensions() const override { return {static_cast<uint32_t>(width_), static_cast<uint32_t>(height_)}; }
    PixelFormat format() const override { return format_; }
    SamplerDesc get_sampler_desc() const override { return sampler_desc_; }
    color get_average_color() const override;

private:
    string uri_;
    int width_{};
    int height_{};
    PixelFormat format_{PixelFormat::RGBA8_SRGB};
    /// The colour space a loader set (set_format): 0 none, 1 linear, 2 sRGB.
    uint8_t color_space_{0};
    /// Levels its data holds (IImage::get_mip_levels), 0 for none of its own.
    uint32_t mip_levels_{0};
    vector<uint8_t> pixels_;
    ImageStatus status_{ImageStatus::Unloaded};
    bool dirty_{false};
    bool persistent_{false};
    SamplerDesc sampler_desc_{};
    /// Kept after the pixels are freed at upload.
    ImageAverages averages_{};
};

} // namespace velk::ui::impl

#endif // VELK_UI_IMAGE_IMPL_H
