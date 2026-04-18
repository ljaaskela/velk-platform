#ifndef VELK_RENDER_INTF_PROGRAM_H
#define VELK_RENDER_INTF_PROGRAM_H

#include <velk/interface/intf_metadata.h>

#include <velk-render/interface/intf_gpu_resource.h>

#include <cstdint>

namespace velk {

class IRenderContext;

/**
 * @brief A GPU shader program: owns a compiled pipeline handle.
 *
 * Narrow marker for shader-program objects participating in the
 * pipeline cache and frame-deferred destruction (via IGpuResource).
 * Per-draw GPU data lives on `IDrawData`; shader sources on
 * `IRasterShader` / `IShaderSnippet`. A concrete material implements
 * whichever of those roles it contributes.
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
};

} // namespace velk

#endif // VELK_RENDER_INTF_PROGRAM_H
