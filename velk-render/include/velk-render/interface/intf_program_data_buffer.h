#ifndef VELK_RENDER_INTF_PROGRAM_DATA_BUFFER_H
#define VELK_RENDER_INTF_PROGRAM_DATA_BUFFER_H

#include <velk-render/interface/intf_buffer.h>

#include <cstddef>

namespace velk {

/**
 * @brief IBuffer specialisation for persistent per-program draw data.
 *
 * Materials own an IProgramDataBuffer (typically obtained via
 * `velk::instance().create<IBuffer>(ClassId::ProgramDataBuffer)`) and
 * return it from `IDrawData::get_data_buffer()`. The buffer owns its
 * own scratch so the material can serialise through `write()` without
 * maintaining a per-instance byte vector. The buffer compares each
 * write against the previously committed bytes and only flags dirty on
 * actual change, so unchanged materials skip GPU re-uploads. The GPU
 * address stays stable across frames until the buffer's data size
 * changes (or it is replaced), which is what lets shape caches hold
 * the address across frames.
 */
class IProgramDataBuffer : public Interface<IProgramDataBuffer, IBuffer>
{
    // No additional members. The data-fill API (`IBuffer::write`) and
    // memcmp-gated write (`IBuffer::write_diff`) live on IBuffer; this
    // interface remains as a typed cap so the renderer can demand a
    // material's data buffer is hive-pooled / observable in the same
    // way as ProgramDataBuffer (consumers don't need an extra method).
};

} // namespace velk

#endif // VELK_RENDER_INTF_PROGRAM_DATA_BUFFER_H
