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
/// `interface_cast<IVkRenderTargetGroup>(p)` to reach the render pass /
/// framebuffer / depth resources without depending on the concrete
/// `VkRenderTargetGroup`.
class IVkRenderTargetGroup
    : public ::velk::Interface<IVkRenderTargetGroup, ::velk::IInterface,
                               VELK_UID("785d4518-1288-490e-adc6-2a44876a46cb")>
{
public:
    virtual void init(::velk::IRenderBackend* backend,
                      ::velk::vector<::velk::IGpuTexture::Ptr> attachments,
                      ::VkRenderPass render_pass,
                      ::VkRenderPass load_render_pass,
                      ::VkFramebuffer framebuffer,
                      ::VkImage depth_image,
                      ::VkImageView depth_view,
                      VmaAllocation depth_allocation,
                      ::VkFormat depth_vk_format,
                      ::velk::uvec2 dimensions,
                      ::velk::PixelFormat color_format,
                      ::velk::DepthFormat depth_format) = 0;

    virtual ::VkRenderPass  vk_render_pass()      const = 0;
    virtual ::VkRenderPass  vk_load_render_pass() const = 0;
    virtual ::VkFramebuffer vk_framebuffer()      const = 0;
    virtual ::VkImage       vk_depth_image()      const = 0;
    virtual ::VkImageView   vk_depth_view()       const = 0;
    virtual VmaAllocation   vk_depth_allocation() const = 0;
    virtual ::VkFormat      vk_depth_format()     const = 0;
    virtual size_t          vk_attachment_count() const = 0;

    /// Whether `begin_pass` already cleared this group in the current frame.
    virtual bool was_cleared_this_frame() const = 0;
    virtual void mark_cleared_this_frame(bool cleared) = 0;
};

/// Vulkan-backed `IRenderTextureGroup`: owns N color attachments
/// (IGpuTexture::Ptr each, allocated via `create_texture` with
/// ColorAttachment usage), the shared render pass + framebuffer, and an
/// optional depth image. Dropping the last Ptr defers the vk handles
/// for destruction; cascading destroys also drop attachment Ptrs.
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
    // IVkRenderTargetGroup
    void init(::velk::IRenderBackend* backend,
              ::velk::vector<::velk::IGpuTexture::Ptr> attachments,
              ::VkRenderPass render_pass,
              ::VkRenderPass load_render_pass,
              ::VkFramebuffer framebuffer,
              ::VkImage depth_image,
              ::VkImageView depth_view,
              VmaAllocation depth_allocation,
              ::VkFormat depth_vk_format,
              ::velk::uvec2 dimensions,
              ::velk::PixelFormat color_format,
              ::velk::DepthFormat depth_format) override;

    ::VkRenderPass  vk_render_pass()      const override { return render_pass_; }
    ::VkRenderPass  vk_load_render_pass() const override { return load_render_pass_; }
    ::VkFramebuffer vk_framebuffer()      const override { return framebuffer_; }
    ::VkImage       vk_depth_image()      const override { return depth_image_; }
    ::VkImageView   vk_depth_view()       const override { return depth_view_; }
    VmaAllocation   vk_depth_allocation() const override { return depth_allocation_; }
    ::VkFormat      vk_depth_format()     const override { return depth_vk_format_; }
    size_t          vk_attachment_count() const override { return attachments_.size(); }
    bool was_cleared_this_frame() const override { return cleared_this_frame_; }
    void mark_cleared_this_frame(bool c) override { cleared_this_frame_ = c; }

private:
    ::velk::IRenderBackend* backend_ = nullptr;
    ::velk::vector<::velk::IGpuTexture::Ptr> attachments_;
    ::VkRenderPass  render_pass_      = VK_NULL_HANDLE;
    ::VkRenderPass  load_render_pass_ = VK_NULL_HANDLE;
    ::VkFramebuffer framebuffer_      = VK_NULL_HANDLE;
    ::VkImage       depth_image_      = VK_NULL_HANDLE;
    ::VkImageView   depth_view_       = VK_NULL_HANDLE;
    VmaAllocation   depth_allocation_ = VK_NULL_HANDLE;
    ::VkFormat      depth_vk_format_  = VK_FORMAT_UNDEFINED;
    ::velk::uvec2       dimensions_{};
    ::velk::PixelFormat format_       = ::velk::PixelFormat::RGBA8;
    ::velk::DepthFormat depth_format_ = ::velk::DepthFormat::None;
    bool cleared_this_frame_ = false;
};

} // namespace velk::vk

#endif // VELK_VK_RENDER_TARGET_GROUP_H
