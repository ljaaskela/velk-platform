#include "vk_gpu_pipeline.h"

namespace velk::vk {

VkGpuPipeline::~VkGpuPipeline()
{
    if (pipeline_ == VK_NULL_HANDLE || backend_ == nullptr) return;
    // Defer here while derived members + vtable are still intact —
    // observer callbacks fire from ~ext::GpuResource after this body
    // returns and would hit the now-pure IVkGpuPipeline virtuals.
    backend_->defer_destroy_gpu_pipeline(this);
}

void VkGpuPipeline::init(IRenderBackend* backend, ::VkPipeline pipeline,
                         ::VkPipelineBindPoint bind_point)
{
    backend_ = backend;
    pipeline_ = pipeline;
    bind_point_ = bind_point;
}

} // namespace velk::vk
