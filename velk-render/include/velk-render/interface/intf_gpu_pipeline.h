#ifndef VELK_RENDER_INTF_GPU_PIPELINE_H
#define VELK_RENDER_INTF_GPU_PIPELINE_H

#include <velk-render/interface/intf_gpu_resource.h>

namespace velk {

/**
 * @brief Backend pipeline (graphics or compute) with managed lifetime.
 *
 * Returned by `IRenderBackend::create_pipeline` /
 * `create_compute_pipeline` and held by the renderer's pipeline
 * cache. Producers reference pipelines as a non-owning
 * `IGpuPipeline*` in `DrawCall::pipeline` / `DispatchCall::pipeline`;
 * the cache keeps them alive across frames. Dropping the last Ptr
 * defers destruction through the backend's frame-completion-marker
 * queue.
 *
 * Native handle access is backend-internal — the Vulkan plugin
 * declares an `IVkGpuPipeline` sibling interface that record loops
 * `interface_cast` to.
 *
 * Chain: IInterface -> IGpuResource -> IGpuPipeline
 */
class IGpuPipeline
    : public Interface<IGpuPipeline, IGpuResource,
                       VELK_UID("bcbafd2a-6520-45f8-89b3-470f404192eb")>
{
};

} // namespace velk

#endif // VELK_RENDER_INTF_GPU_PIPELINE_H
