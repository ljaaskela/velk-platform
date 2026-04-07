#ifndef VELK_RENDER_INTF_TEXTURE_H
#define VELK_RENDER_INTF_TEXTURE_H

#include <velk-render/interface/intf_gpu_resource.h>
#include <velk-render/interface/intf_render_backend.h> // for PixelFormat

#include <cstdint>

namespace velk {

/**
 * @brief A 2D image source bindable as a GPU texture.
 *
 * `ITexture` is the unified concept that replaces the older
 * `ITextureProvider`: it covers both dynamic CPU-resident sources (e.g. a
 * glyph atlas that grows as glyphs are rasterized) and static images that
 * have been uploaded once and now live only on the GPU.
 *
 * Two lifecycles fit the same interface:
 *
 * - **Dynamic source** (e.g. font glyph atlas): keeps `get_pixels()`
 *   valid, sets `is_dirty()` after mutation, and the renderer re-uploads
 *   on the next frame.
 * - **Static image** (e.g. a decoded png): `get_pixels()` returns the
 *   decoded RGBA bytes once, `is_dirty()` returns true on first observation,
 *   the renderer uploads, the texture clears its dirty flag and may free
 *   its CPU pixels (so `get_pixels()` returns nullptr afterwards).
 *
 * `ITexture` is a `IGpuResource`, so observers (typically renderers) are
 * notified when the texture is destroyed and can defer destruction of the
 * GPU handles they associated with it.
 *
 * Chain: IInterface -> IGpuResource -> ITexture
 */
class ITexture : public Interface<ITexture, IGpuResource>
{
public:
    /** @brief Texture width in pixels. */
    virtual int width() const = 0;

    /** @brief Texture height in pixels. */
    virtual int height() const = 0;

    /** @brief Pixel format. */
    virtual PixelFormat format() const = 0;

    /**
     * @brief Returns the CPU pixel data, or nullptr if pixels are not
     *        available on the CPU (e.g. a static image whose pixels have
     *        been freed after upload, or a GPU-only texture).
     */
    virtual const uint8_t* get_pixels() const = 0;

    /**
     * @brief Stride in bytes between consecutive rows of pixel data.
     *        Returns 0 to indicate tightly packed (`width * bytes_per_pixel`).
     */
    virtual uint32_t row_pitch() const { return 0; }

    /**
     * @brief Returns true if `get_pixels()` content has changed and the
     *        renderer should re-upload to the GPU on the next frame.
     */
    virtual bool is_dirty() const = 0;

    /**
     * @brief Called by the renderer after re-uploading from `get_pixels()`.
     *        Implementations may free their CPU pixel buffer here if they
     *        no longer need it.
     */
    virtual void clear_dirty() = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_TEXTURE_H
