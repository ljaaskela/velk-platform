#ifndef VELK_RENDER_INTF_IMAGE_H
#define VELK_RENDER_INTF_IMAGE_H

#include <velk/interface/resource/intf_resource.h>
#include <velk/string_view.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/render_types.h>

#include <cstdint>

namespace velk {

/// Lifecycle status of an `IImage`.
enum class ImageStatus : uint8_t
{
    Unloaded, ///< Resource exists but no decode has been attempted yet.
    Loading,  ///< Decode is in progress (future async loading).
    Loaded,   ///< Decoded successfully; the texture surface is valid.
    Failed,   ///< Decode was attempted and failed.
};

/**
 * @brief A 2D image resource identified by URI, with a load lifecycle.
 *
 * `IImage` is the resource concept: an image known by URI, cached and
 * deduplicated by the resource store, optionally pinned via the persistence
 * flag inherited from `IResource`. It is intentionally minimal: dimensions
 * and format live on `ISurface`, pixel data on `IBuffer`.
 *
 * The concrete `Image` class produced by the image plugin's decoder
 * implements `IImage`, `ISurface`, and `IBuffer`. Apps holding an
 * `IImage::Ptr` can `interface_cast<ISurface>(image)` to get the binding
 * surface for materials.
 *
 * Sync v1 only ever produces `Loaded` or `Failed` status. `Unloaded` and
 * `Loading` exist in the enum so consumers can be written defensively for
 * future lazy and async loading without an interface change.
 *
 * Chain: IInterface -> IResource -> IImage
 */
class IImage : public Interface<IImage, IResource>
{
public:
    /** @brief Returns the current load status. */
    virtual ImageStatus status() const = 0;

    /**
     * @brief Overrides the sampler desc used when this image is uploaded.
     *
     * Loaders that know more than the image plugin (e.g. the glTF
     * importer reading a `sampler` record) call this between decode and
     * first observation by the renderer. Has no effect once the texture
     * has been uploaded — the sampler captured at upload time is final.
     */
    virtual void set_sampler_desc(const SamplerDesc& desc) = 0;

    /**
     * @brief Overrides the pixel format (color space) used when this image is
     *        uploaded.
     *
     * Loaders that know an image's role call this between decode and first
     * observation by the renderer — e.g. the glTF importer retagging normal /
     * metallic-roughness / occlusion maps as linear (`RGBA8`) instead of the
     * sRGB default, which glTF requires for non-color data. The pixel bytes are
     * unchanged; only the GPU's sample-time color-space handling differs. No
     * effect once the texture has been uploaded.
     */
    virtual void set_format(PixelFormat format) = 0;

    /**
     * @brief Initializes the image directly from decoded RGBA pixel data,
     *        bypassing any bytes decoder.
     *
     * Used by importers that need to synthesize textures whose bytes are
     * not on disk (e.g. the glTF spec/gloss converter that derives a
     * roughness texture from a specularGlossinessTexture). The pixel
     * buffer is copied internally; `pixels` must remain valid only for
     * the duration of the call. After this returns, `status()` is
     * `Loaded`.
     */
    virtual void init_from_pixels(string_view uri, int width, int height,
                                  PixelFormat format,
                                  const uint8_t* pixels, size_t pixel_size) = 0;
};

/**
 * @brief Encodes an `IImage` to disk in a chosen file format.
 *
 * Decoupled from `IImage` so that encoding backends can be swapped or
 * extended (additional formats, host-side metadata, alternative
 * compressors) without touching every image producer. Instantiate via
 * `velk::instance().create<IImageWriter>(<encoder ClassId>)`; the image
 * plugin registers a default implementation.
 *
 * Chain: IInterface -> IImageWriter
 */
class IImageWriter : public Interface<IImageWriter>
{
public:
    /**
     * @brief Saves @p image as a PNG file at @p path.
     * @return true on success; false if the image format is unsupported,
     *         the image has no readable pixel data (e.g. pixels were
     *         freed after GPU upload), or the file could not be written.
     */
    virtual bool save_png(const IImage& image, string_view path) const = 0;

    /**
     * @brief Saves raw pixel data as a PNG file. Used by callers that
     *        already have bytes (e.g. GPU readbacks) and don't want to
     *        construct an `IImage` wrapper just to save them.
     */
    virtual bool save_png(int width, int height, PixelFormat format,
                          const uint8_t* pixels, size_t pixel_size,
                          string_view path) const = 0;

    /**
     * @brief Saves @p image as a Radiance .hdr file at @p path.
     *
     * Targets float-formatted images (RGBA16F, RGBA32F). Returns false
     * for integer formats (use `save_png` for those) or if pixel data
     * is missing / the file cannot be written.
     */
    virtual bool save_hdr(const IImage& image, string_view path) const = 0;

    /**
     * @brief Saves raw pixel data as a Radiance .hdr file.
     */
    virtual bool save_hdr(int width, int height, PixelFormat format,
                          const uint8_t* pixels, size_t pixel_size,
                          string_view path) const = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_IMAGE_H
