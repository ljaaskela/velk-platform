#ifndef VELK_RENDER_INTF_DRAW_DATA_H
#define VELK_RENDER_INTF_DRAW_DATA_H

#include <velk/interface/intf_metadata.h>

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
     * @return ReturnValue::Success on success, ReturnValue::Fail on error.
     */
    virtual ReturnValue write_draw_data(void* out, size_t size) const = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_DRAW_DATA_H
