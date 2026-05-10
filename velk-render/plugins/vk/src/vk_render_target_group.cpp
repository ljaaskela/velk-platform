#include "vk_render_target_group.h"

namespace velk::vk {

VkRenderTargetGroup::~VkRenderTargetGroup()
{
    if (render_pass_ == VK_NULL_HANDLE && framebuffer_ == VK_NULL_HANDLE) {
        return;
    }
    if (backend_) {
        backend_->defer_destroy_gpu_render_target_group(this);
    }
}

void VkRenderTargetGroup::init(::velk::IRenderBackend* backend,
                               ::velk::vector<::velk::IGpuTexture::Ptr> attachments,
                               ::velk::IGpuTexture::Ptr depth_attachment,
                               ::VkRenderPass render_pass,
                               ::VkRenderPass load_render_pass,
                               ::VkFramebuffer framebuffer,
                               ::VkFormat depth_vk_format,
                               ::velk::uvec2 dimensions,
                               ::velk::PixelFormat color_format,
                               ::velk::DepthFormat depth_format)
{
    backend_           = backend;
    attachments_       = std::move(attachments);
    depth_attachment_  = std::move(depth_attachment);
    render_pass_       = render_pass;
    load_render_pass_  = load_render_pass;
    framebuffer_       = framebuffer;
    depth_vk_format_   = depth_vk_format;
    dimensions_        = dimensions;
    format_            = color_format;
    depth_format_      = depth_format;
}

} // namespace velk::vk
