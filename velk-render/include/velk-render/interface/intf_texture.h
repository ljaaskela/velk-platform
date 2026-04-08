#ifndef VELK_RENDER_INTF_TEXTURE_H
#define VELK_RENDER_INTF_TEXTURE_H

#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_render_backend.h> // for PixelFormat

#include <cstdint>

namespace velk {

/**
 * @brief A 2D image source bindable as a GPU texture.
 *
 * `ITexture` specializes `IBuffer` for image-shaped resources. The CPU-side
 * lifecycle (size, data, dirty, observer) is inherited unchanged; the
 * subtype only adds image metadata: dimensions, pixel format, optional row
 * pitch.
 *
 * Two lifecycles fit the same interface (inherited from IBuffer):
 *
 * - **Dynamic source** (e.g. font glyph atlas): keeps `get_data()` valid,
 *   sets `is_dirty()` after mutation, and the renderer re-uploads on the
 *   next frame.
 * - **Static image** (e.g. a decoded png): `get_data()` returns the decoded
 *   pixel bytes once, `is_dirty()` returns true on first observation, the
 *   renderer uploads, the texture clears its dirty flag and may free its
 *   CPU pixels (so `get_data()` returns nullptr afterwards).
 *
 * Chain: IInterface -> IGpuResource -> IBuffer -> ITexture
 */
class ITexture : public Interface<ITexture, IBuffer>
{
public:
    /** @brief Texture width in pixels. */
    virtual int width() const = 0;

    /** @brief Texture height in pixels. */
    virtual int height() const = 0;

    /** @brief Pixel format. */
    virtual PixelFormat format() const = 0;

    /**
     * @brief Stride in bytes between consecutive rows of pixel data.
     *        Returns 0 to indicate tightly packed (`width * bytes_per_pixel`).
     */
    virtual uint32_t row_pitch() const { return 0; }
};

} // namespace velk

#endif // VELK_RENDER_INTF_TEXTURE_H
