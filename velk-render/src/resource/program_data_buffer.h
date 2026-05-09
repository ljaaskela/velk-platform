#ifndef VELK_RENDER_PROGRAM_DATA_BUFFER_H
#define VELK_RENDER_PROGRAM_DATA_BUFFER_H

#include <velk-render/ext/gpu_buffer.h>
#include <velk-render/interface/intf_gpu_buffer.h>
#include <velk-render/interface/intf_program_data_buffer.h>
#include <velk-render/plugin.h>

namespace velk::impl {

/**
 * @brief Concrete `IProgramDataBuffer`. Same byte-blob shape as
 *        `impl::GpuBuffer`; the typed cap exists so material APIs
 *        can demand a hive-pooled / observable program-data buffer
 *        specifically. All behaviour inherited from `ext::GpuBuffer`.
 */
class ProgramDataBuffer
    : public ::velk::ext::GpuBuffer<ProgramDataBuffer,
                                    ::velk::IProgramDataBuffer,
                                    ::velk::IGpuBuffer,
                                    ::velk::IGpuBufferStorageOwner>
{
public:
    VELK_CLASS_UID(::velk::ClassId::ProgramDataBuffer, "ProgramDataBuffer");
};

} // namespace velk::impl

#endif // VELK_RENDER_PROGRAM_DATA_BUFFER_H
