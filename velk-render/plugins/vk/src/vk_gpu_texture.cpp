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
                                         const ::velk::SamplerDesc& sampler)
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
    set_gpu_handle(::velk::GpuResourceKey::Default,
                   static_cast<uint64_t>(bindless_index));
}

VkSurfaceTexture::~VkSurfaceTexture()
{
    // Backend explicitly tears down via release() before the surface
    // object is destroyed — no observer-deferred destroy path.
}

void VkSurfaceTexture::init(::velk::IRenderBackend* backend,
                            uint64_t surface_id,
                            ::VkImage image, ::VkImageView view,
                            VmaAllocation allocation,
                            ::velk::uvec2 dimensions,
                            ::velk::PixelFormat format)
{
    backend_       = backend;
    surface_id_    = surface_id;
    image_         = image;
    view_          = view;
    allocation_    = allocation;
    dimensions_    = dimensions;
    format_        = format;
    current_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    cleared_this_frame_ = false;
    // gpu_handle stays 0 — the surface composite isn't bindless-sampled
    // through the heap. (CameraPipeline doesn't sample it; producers
    // render into it via record_begin_rendering or blit into it via
    // record_blit_to_texture.)
}

void VkSurfaceTexture::release(::VkDevice device, VmaAllocator allocator)
{
    if (view_) vkDestroyImageView(device, view_, nullptr);
    if (image_) vmaDestroyImage(allocator, image_, allocation_);
    image_      = VK_NULL_HANDLE;
    view_       = VK_NULL_HANDLE;
    allocation_ = VK_NULL_HANDLE;
}

} // namespace velk::vk
