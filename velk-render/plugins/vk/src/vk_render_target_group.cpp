#include "vk_render_target_group.h"

namespace velk::vk {

VkRenderTargetGroup::~VkRenderTargetGroup()
{
    // The group owns no GPU resources of its own (color/depth attachments
    // are IGpuTexture::Ptrs that defer their own destruction). This call
    // only unregisters the group from the backend's live-group tracking.
    if (backend_) {
        backend_->defer_destroy_gpu_render_target_group(this);
    }
}

void VkRenderTargetGroup::init(::velk::IRenderBackend* backend,
                               ::velk::vector<::velk::IGpuTexture::Ptr> attachments,
                               ::velk::IGpuTexture::Ptr depth_attachment,
                               ::VkFormat depth_vk_format,
                               ::velk::uvec2 dimensions,
                               ::velk::PixelFormat color_format,
                               ::velk::DepthFormat depth_format)
{
    backend_           = backend;
    attachments_       = std::move(attachments);
    depth_attachment_  = std::move(depth_attachment);
    depth_vk_format_   = depth_vk_format;
    dimensions_        = dimensions;
    format_            = color_format;
    depth_format_      = depth_format;
}

} // namespace velk::vk
