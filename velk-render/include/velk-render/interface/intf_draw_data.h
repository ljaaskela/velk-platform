#ifndef VELK_RENDER_INTF_DRAW_DATA_H
#define VELK_RENDER_INTF_DRAW_DATA_H

#include <velk/interface/intf_metadata.h>

#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_texture_resolver.h>

#include <cstddef>

namespace velk {

/**
 * @brief Contract for objects that contribute a per-draw GPU data blob
 *        alongside a compiled pipeline.
 *
 * The renderer writes `get_draw_data_size()` bytes of per-draw state
 * via `write_draw_data()` into the staging buffer immediately after
 * the `DrawDataHeader`. The shader reads this data via
 * `buffer_reference` from the draw data pointer.
 *
 * Typically implemented alongside `IMaterial` (material-side data)
 * but kept as a separate interface so programs that only contribute
 * shader code (no per-draw data) don't have to implement empty stubs.
 */
class IDrawData : public Interface<IDrawData>
{
public:
    /** @brief Returns the size in bytes of this object's per-draw GPU data. */
    virtual size_t get_draw_data_size() const = 0;

    /**
     * @brief Writes per-draw GPU data into the staging buffer.
     * @param out  Destination buffer (immediately after DrawDataHeader).
     * @param size Buffer size in bytes (equals get_draw_data_size()).
     * @param resolver Optional bindless TextureId resolver for materials
     *                 that embed texture ids in their UBO. Callers that
     *                 know the material samples no textures may pass null.
     * @return ReturnValue::Success on success, ReturnValue::Fail on error.
     */
    virtual ReturnValue write_draw_data(void* out, size_t size,
                                        ITextureResolver* resolver = nullptr) const = 0;

    /**
     * @brief Returns a persistent IBuffer holding this program's current
     *        draw-data bytes. Stable GPU address across frames (as long
     *        as the data size doesn't change), so consumers caching the
     *        address in shape records remain valid across frames.
     *
     * Materials implementing this method own the buffer internally; the
     * renderer picks it up through the standard IBuffer upload path
     * (is_dirty / clear_dirty / set_gpu_address) and holds a strong ref
     * for the duration of any frame that captured its address, so the
     * material can reset or replace the buffer at any time without
     * synchronising with the renderer — the IGpuResource observer
     * mechanism handles deferred GPU-handle destruction.
     *
     * Default returns nullptr for programs that have no persistent
     * buffer; such programs fall back to per-frame serialisation via
     * `write_draw_data` into the renderer's frame scratch arena.
     *
     * @param resolver Optional bindless TextureId resolver; forwarded
     *                 to `write_draw_data` so materials can embed ids
     *                 in their UBO.
     */
    virtual IBuffer::Ptr get_data_buffer(ITextureResolver* resolver = nullptr) { (void)resolver; return nullptr; }
};

} // namespace velk

#endif // VELK_RENDER_INTF_DRAW_DATA_H
