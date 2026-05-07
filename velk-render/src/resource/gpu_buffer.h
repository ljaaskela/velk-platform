#ifndef VELK_RENDER_RESOURCE_GPU_BUFFER_H
#define VELK_RENDER_RESOURCE_GPU_BUFFER_H

#include <velk-render/ext/gpu_buffer.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/plugin.h>

namespace velk::impl {

/**
 * @brief Generic byte-blob `IBuffer`. Hive-pooled via
 *        `ClassId::GpuBuffer`. Composed by any owner that needs
 *        persistent GPU-side storage. All behaviour (data_/pending_/
 *        dirty_, IBuffer methods) inherited from `ext::GpuBuffer`.
 */
class GpuBuffer : public ::velk::ext::GpuBuffer<GpuBuffer, ::velk::IBuffer>
{
public:
    VELK_CLASS_UID(::velk::ClassId::GpuBuffer, "GpuBuffer");
};

} // namespace velk::impl

#endif // VELK_RENDER_RESOURCE_GPU_BUFFER_H
