#include "vk_gpu_texture.h"

namespace velk::vk {

VkGpuTexture::~VkGpuTexture()
{
    if (image_ == VK_NULL_HANDLE || backend_ == nullptr) return;
    // Defer here while derived members + vtable are still intact —
    // observer callbacks fire from ~ext::GpuResource after this body
    // returns and would hit the now-pure IVkGpuTexture virtuals.
    backend_->defer_destroy_gpu_texture(this);
}

void VkGpuTexture::init_sampled(::velk::IRenderBackend* backend,
                                ::VkImage image, ::VkImageView view,
                                VmaAllocation allocation,
                                uint32_t bindless_index, uint32_t mip_levels,
                                ::VkImageLayout initial_layout,
                                ::velk::uvec2 dimensions,
                                ::velk::PixelFormat format,
                                const ::velk::SamplerDesc& sampler)
{
    backend_        = backend;
    image_          = image;
    view_           = view;
    allocation_     = allocation;
    bindless_index_ = bindless_index;
    mip_levels_     = mip_levels;
    current_layout_ = initial_layout;
    dimensions_     = dimensions;
    format_         = format;
    sampler_        = sampler;
    set_gpu_handle(::velk::GpuResourceKey::Default,
                   static_cast<uint64_t>(bindless_index));
}

VkRenderTexture::~VkRenderTexture()
{
    if (image_ == VK_NULL_HANDLE || backend_ == nullptr) return;
    backend_->defer_destroy_gpu_texture(this);
}

void VkRenderTexture::init_render_target(::velk::IRenderBackend* backend,
                                         ::VkImage image, ::VkImageView view,
                                         VmaAllocation allocation,
                                         uint32_t bindless_index,
                                         uint32_t mip_levels,
                                         ::VkImageLayout initial_layout,
                                         ::velk::uvec2 dimensions,
                                         ::velk::PixelFormat format,
                                         const ::velk::SamplerDesc& sampler,
                                         ::VkFramebuffer framebuffer,
                                         ::VkRenderPass render_pass,
                                         ::VkRenderPass load_render_pass)
{
    backend_           = backend;
    image_             = image;
    view_              = view;
    allocation_        = allocation;
    bindless_index_    = bindless_index;
    mip_levels_        = mip_levels;
    current_layout_    = initial_layout;
    dimensions_        = dimensions;
    format_            = format;
    sampler_           = sampler;
    framebuffer_       = framebuffer;
    render_pass_       = render_pass;
    load_render_pass_  = load_render_pass;
    set_gpu_handle(::velk::GpuResourceKey::Default,
                   static_cast<uint64_t>(bindless_index));
}

} // namespace velk::vk
