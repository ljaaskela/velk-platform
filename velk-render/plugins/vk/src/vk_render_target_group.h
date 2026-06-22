#ifndef VELK_VK_RENDER_TARGET_GROUP_H
#define VELK_VK_RENDER_TARGET_GROUP_H

#include <velk/vector.h>

#include <velk-render/ext/gpu_resource.h>
#include <velk-render/interface/intf_gpu_texture.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_texture_group.h>
#include <velk-render/platform.h>
#include <vma/vk_mem_alloc.h>
#include <volk/volk.h>

namespace velk::vk {

/// Backend-internal sibling of `IRenderTextureGroup` exposing Vulkan
/// handles. Backend code uses
/// `interface_cast<IVkRenderTargetGroup>(p)` to reach the depth format /
/// attachment info without depending on the concrete
/// `VkRenderTargetGroup`.
class IVkRenderTargetGroup
    : public ::velk::Interface<IVkRenderTargetGroup, ::velk::IInterface,
                               VELK_UID("785d4518-1288-490e-adc6-2a44876a46cb")>
{
public:
    virtual void init(::velk::IRenderBackend* backend,
                      ::velk::vector<::velk::IGpuTexture::Ptr> attachments,
                      ::velk::IGpuTexture::Ptr depth_attachment,
                      ::VkFormat depth_vk_format,
                      ::velk::uvec2 dimensions,
                      ::velk::PixelFormat color_format,
                      ::velk::DepthFormat depth_format) = 0;

    virtual ::VkFormat      vk_depth_format()     const = 0;
    virtual size_t          vk_attachment_count() const = 0;

    /// Whether `begin_pass` already cleared this group in the current frame.
    virtual bool was_cleared_this_frame() const = 0;
    virtual void mark_cleared_this_frame(bool cleared) = 0;
};

/// Vulkan-backed `IRenderTextureGroup`: owns N color attachments
/// (IGpuTexture::Ptr each, allocated via `create_texture` with
/// ColorAttachment usage) and an optional depth attachment (also an
/// IGpuTexture::Ptr). Rendered into via dynamic rendering, so the group
/// holds no render pass / framebuffer. Dropping the last Ptr cascades to
/// the attachment Ptrs, each deferring its own destruction.
class VkRenderTargetGroup
    : public ::velk::ext::GpuResource<VkRenderTargetGroup,
                                      ::velk::IRenderTextureGroup,
                                      IVkRenderTargetGroup>
{
public:
    VELK_CLASS_UID(::velk::ClassId::VkRenderTargetGroup, "VkRenderTargetGroup");

    VkRenderTargetGroup() = default;
    ~VkRenderTargetGroup() override;

    // IGpuResource
    ::velk::GpuResourceType get_type() const override
    {
        return ::velk::GpuResourceType::Texture;
    }

    // ISurface (shared by IRenderTarget chain)
    ::velk::uvec2       get_dimensions()   const override { return dimensions_; }
    ::velk::PixelFormat format()           const override { return format_; }
    ::velk::SamplerDesc get_sampler_desc() const override { return {}; }

    // IRenderTarget
    ::velk::DepthFormat get_depth_format() const override { return depth_format_; }
    void set_depth_format(::velk::DepthFormat df) override { depth_format_ = df; }
    void set_size(uint32_t w, uint32_t h) override { dimensions_ = {w, h}; }
    void set_format(::velk::PixelFormat f) override { format_ = f; }

    // IRenderTextureGroup
    size_t attachment_count() const override { return attachments_.size(); }
    void clear_attachments() override { attachments_.clear(); }
    ::velk::IGpuTexture* attachment_texture(uint32_t index) const override
    {
        return index < attachments_.size() ? attachments_[index].get() : nullptr;
    }
    ::velk::IGpuTexture* depth_attachment() const override
    {
        return depth_attachment_.get();
    }
    // IVkRenderTargetGroup
    void init(::velk::IRenderBackend* backend,
              ::velk::vector<::velk::IGpuTexture::Ptr> attachments,
              ::velk::IGpuTexture::Ptr depth_attachment,
              ::VkFormat depth_vk_format,
              ::velk::uvec2 dimensions,
              ::velk::PixelFormat color_format,
              ::velk::DepthFormat depth_format) override;

    ::VkFormat      vk_depth_format()     const override { return depth_vk_format_; }
    size_t          vk_attachment_count() const override { return attachments_.size(); }
    bool was_cleared_this_frame() const override { return cleared_this_frame_; }
    void mark_cleared_this_frame(bool c) override { cleared_this_frame_ = c; }

private:
    ::velk::IRenderBackend* backend_ = nullptr;
    ::velk::vector<::velk::IGpuTexture::Ptr> attachments_;
    /// Depth attachment as a real IGpuTexture::Ptr so
    /// `record_begin_rendering` can bind it via the same shape as
    /// color attachments. Dropping the group's last Ptr cascades to
    /// the depth wrapper's destructor, which defers via the standard
    /// IGpuTexture observer chain. nullptr when DepthFormat::None.
    ::velk::IGpuTexture::Ptr depth_attachment_;
    ::VkFormat      depth_vk_format_  = VK_FORMAT_UNDEFINED;
    ::velk::uvec2       dimensions_{};
    ::velk::PixelFormat format_       = ::velk::PixelFormat::RGBA8;
    ::velk::DepthFormat depth_format_ = ::velk::DepthFormat::None;
    bool cleared_this_frame_ = false;
};

} // namespace velk::vk

#endif // VELK_VK_RENDER_TARGET_GROUP_H
