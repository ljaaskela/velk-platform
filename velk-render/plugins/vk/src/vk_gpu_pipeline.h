#ifndef VELK_VK_GPU_PIPELINE_H
#define VELK_VK_GPU_PIPELINE_H

#include <velk-render/ext/gpu_resource.h>
#include <velk-render/interface/intf_gpu_pipeline.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/platform.h>
#include <volk/volk.h>

namespace velk::vk {

/**
 * @brief Backend-internal sibling of `IGpuPipeline` exposing the
 *        Vulkan handle. Record loops `interface_cast` to this to
 *        extract `VkPipeline` without depending on the concrete
 *        `VkGpuPipeline` type.
 */
class IVkGpuPipeline
    : public ::velk::Interface<IVkGpuPipeline, ::velk::IInterface,
                               VELK_UID("92cdeaa2-6ae4-4fc1-a5e5-6335136b1ae5")>
{
public:
    virtual void init(::velk::IRenderBackend* backend, ::VkPipeline pipeline,
                      ::VkPipelineBindPoint bind_point) = 0;
    virtual ::VkPipeline          vk_pipeline()   const = 0;
    virtual ::VkPipelineBindPoint vk_bind_point() const = 0;
};

/**
 * @brief Vulkan-backed `IGpuPipeline`. Owns one `VkPipeline`
 *        (graphics or compute) and its bind point.
 *
 * Constructed by `VkBackend::create_pipeline` /
 * `create_compute_pipeline`. Destruction defers
 * `vkDestroyPipeline` to the backend's deferred-free queue keyed
 * by the current pending-frame-completion marker.
 */
class VkGpuPipeline
    : public ::velk::ext::GpuResource<VkGpuPipeline,
                                      ::velk::IGpuPipeline, IVkGpuPipeline>
{
public:
    VELK_CLASS_UID(::velk::ClassId::VkGpuPipeline, "VkGpuPipeline");

    VkGpuPipeline() = default;
    ~VkGpuPipeline() override;

    // IGpuResource
    ::velk::GpuResourceType get_type() const override
    {
        return ::velk::GpuResourceType::Program;
    }

    // IVkGpuPipeline
    void init(::velk::IRenderBackend* backend, ::VkPipeline pipeline,
              ::VkPipelineBindPoint bind_point) override;
    ::VkPipeline          vk_pipeline()   const override { return pipeline_; }
    ::VkPipelineBindPoint vk_bind_point() const override { return bind_point_; }

private:
    ::velk::IRenderBackend* backend_ = nullptr;
    ::VkPipeline          pipeline_ = VK_NULL_HANDLE;
    ::VkPipelineBindPoint bind_point_ = VK_PIPELINE_BIND_POINT_GRAPHICS;
};

} // namespace velk::vk

#endif // VELK_VK_GPU_PIPELINE_H
