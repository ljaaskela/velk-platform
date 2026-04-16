#ifndef VELK_RENDER_INTF_PROGRAM_H
#define VELK_RENDER_INTF_PROGRAM_H

#include <velk/interface/intf_metadata.h>

#include <velk-render/interface/intf_gpu_resource.h>

#include <cstddef>
#include <cstdint>

namespace velk {

class IRenderContext;

/**
 * @brief A GPU shader program: a pipeline handle plus per-draw GPU data.
 *
 * The renderer binds the pipeline returned by `get_pipeline_handle()` and
 * writes `gpu_data_size()` bytes of per-draw state via `write_gpu_data()`
 * immediately after the `DrawDataHeader` in the staging buffer. The shader
 * reads this data via `buffer_reference` from the draw data pointer.
 *
 * The pipeline handle is a GPU resource and participates in the
 * `IGpuResource` observer protocol for frame-deferred destruction: when the
 * program is destroyed, the renderer queues the pipeline for destruction
 * after the frames currently in flight have finished.
 *
 * Chain: IInterface -> IGpuResource -> IProgram
 */
class IProgram : public Interface<IProgram, IGpuResource>
{
public:
    GpuResourceType get_type() const override { return GpuResourceType::Program; }

    /** @brief Returns the pipeline handle, compiling lazily if needed. */
    virtual uint64_t get_pipeline_handle(IRenderContext& ctx) = 0;

    /**
     * @brief Stores the compiled pipeline handle.
     *
     * Called by the render context after pipeline compilation. Implementations
     * cache the handle for subsequent `get_pipeline_handle()` calls.
     */
    virtual void set_pipeline_handle(uint64_t handle) = 0;

    /** @brief Returns the size in bytes of this program's per-draw GPU data. */
    virtual size_t gpu_data_size() const = 0;

    /**
     * @brief Writes per-draw GPU data into the staging buffer.
     * @param out  Destination buffer (immediately after DrawDataHeader).
     * @param size Buffer size in bytes (equals gpu_data_size()).
     * @return ReturnValue::Success on success, ReturnValue::Fail on error.
     */
    virtual ReturnValue write_gpu_data(void* out, size_t size) const = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_PROGRAM_H
