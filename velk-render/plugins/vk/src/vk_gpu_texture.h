#ifndef VELK_VK_GPU_TEXTURE_H
#define VELK_VK_GPU_TEXTURE_H

#include <velk-render/ext/gpu_resource.h>
#include <velk-render/interface/intf_gpu_texture.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_target.h>
#include <velk-render/platform.h>
#include <vma/vk_mem_alloc.h>
#include <volk/volk.h>

namespace velk::vk {

/// Backend-internal sibling of `IGpuTexture` exposing Vulkan handles.
/// Backend code uses `interface_cast<IVkGpuTexture>(p)` to reach the
/// VkImage / VkImageView / VmaAllocation / framebuffer / render passes
/// without depending on the concrete `VkGpuTexture` / `VkRenderTexture`
/// types.
class IVkGpuTexture
    : public ::velk::Interface<IVkGpuTexture, ::velk::IInterface,
                               VELK_UID("288bea31-feea-4d78-acd8-2d695bb5b67c")>
{
public:
    /// Initializes a sample-only texture (no framebuffer / render pass).
    virtual void init_sampled(::velk::IRenderBackend* backend,
                              ::VkImage image, ::VkImageView view,
                              VmaAllocation allocation,
                              uint32_t bindless_index, uint32_t mip_levels,
                              ::VkImageLayout initial_layout,
                              ::velk::uvec2 dimensions,
                              ::velk::PixelFormat format,
                              const ::velk::SamplerDesc& sampler) = 0;

    /// Initializes a renderable texture (carries framebuffer + render passes
    /// for single-attachment RTT, or null for MRT-group attachments which
    /// share the group's framebuffer / render pass).
    virtual void init_render_target(::velk::IRenderBackend* backend,
                                    ::VkImage image, ::VkImageView view,
                                    VmaAllocation allocation,
                                    uint32_t bindless_index, uint32_t mip_levels,
                                    ::VkImageLayout initial_layout,
                                    ::velk::uvec2 dimensions,
                                    ::velk::PixelFormat format,
                                    const ::velk::SamplerDesc& sampler,
                                    ::VkFramebuffer framebuffer,
                                    ::VkRenderPass render_pass,
                                    ::VkRenderPass load_render_pass) = 0;

    virtual ::VkImage      vk_image()           const = 0;
    virtual ::VkImageView  vk_view()            const = 0;
    virtual VmaAllocation  vk_allocation()      const = 0;
    virtual uint32_t       vk_bindless_index()  const = 0;
    virtual uint32_t       vk_mip_levels()      const = 0;

    /// Render-pass / framebuffer accessors. Return `VK_NULL_HANDLE` for
    /// sample-only textures and for MRT-group attachments (group owns the
    /// framebuffer / render pass instead).
    virtual ::VkFramebuffer vk_framebuffer()      const = 0;
    virtual ::VkRenderPass  vk_render_pass()      const = 0;
    virtual ::VkRenderPass  vk_load_render_pass() const = 0;

    /// Live tracking of the image's current layout. begin_pass / blit_*
    /// consult and update this so cross-pass ops emit correct barriers.
    virtual ::VkImageLayout vk_current_layout()    const = 0;
    virtual void            set_vk_current_layout(::VkImageLayout) = 0;

    /// Whether `begin_pass` already cleared this texture in the current
    /// frame; flipped from CLEAR to LOAD on subsequent passes.
    virtual bool was_cleared_this_frame() const = 0;
    virtual void mark_cleared_this_frame(bool cleared) = 0;

    /// True when this texture is a per-surface composite intermediate.
    /// Used by the cmd-buffer recorder to apply multi-view loadOp
    /// override and dirty tracking, and by `end_frame` to emit the
    /// composite-to-swap blit. False on regular sampled / RTT textures.
    virtual bool is_swap_composite() const = 0;

    /// Surface id this composite belongs to (only meaningful when
    /// `is_swap_composite()` returns true).
    virtual uint64_t swap_surface_id() const = 0;
};

/// Vulkan-backed sample-only `IGpuTexture` (TextureUsage::Sampled /
/// Storage). No render-pass / framebuffer state.
class VkGpuTexture
    : public ::velk::ext::GpuResource<VkGpuTexture,
                                      ::velk::IGpuTexture, IVkGpuTexture>
{
public:
    VELK_CLASS_UID(::velk::ClassId::VkGpuTexture, "VkGpuTexture");

    VkGpuTexture() = default;
    ~VkGpuTexture() override;

    // IGpuResource
    ::velk::GpuResourceType get_type() const override
    {
        return ::velk::GpuResourceType::Texture;
    }

    // ISurface
    ::velk::uvec2       get_dimensions()   const override { return dimensions_; }
    ::velk::PixelFormat format()           const override { return format_; }
    ::velk::SamplerDesc get_sampler_desc() const override { return sampler_; }

    // IVkGpuTexture
    void init_sampled(::velk::IRenderBackend* backend,
                      ::VkImage image, ::VkImageView view,
                      VmaAllocation allocation,
                      uint32_t bindless_index, uint32_t mip_levels,
                      ::VkImageLayout initial_layout,
                      ::velk::uvec2 dimensions,
                      ::velk::PixelFormat format,
                      const ::velk::SamplerDesc& sampler) override;

    void init_render_target(::velk::IRenderBackend*, ::VkImage, ::VkImageView,
                            VmaAllocation, uint32_t, uint32_t,
                            ::VkImageLayout, ::velk::uvec2, ::velk::PixelFormat,
                            const ::velk::SamplerDesc&, ::VkFramebuffer,
                            ::VkRenderPass, ::VkRenderPass) override
    { /* not supported on sample-only impl */ }

    ::VkImage      vk_image()           const override { return image_; }
    ::VkImageView  vk_view()            const override { return view_; }
    VmaAllocation  vk_allocation()      const override { return allocation_; }
    uint32_t       vk_bindless_index()  const override { return bindless_index_; }
    uint32_t       vk_mip_levels()      const override { return mip_levels_; }
    ::VkFramebuffer vk_framebuffer()      const override { return VK_NULL_HANDLE; }
    ::VkRenderPass  vk_render_pass()      const override { return VK_NULL_HANDLE; }
    ::VkRenderPass  vk_load_render_pass() const override { return VK_NULL_HANDLE; }
    ::VkImageLayout vk_current_layout()   const override { return current_layout_; }
    void set_vk_current_layout(::VkImageLayout l) override { current_layout_ = l; }
    bool was_cleared_this_frame() const override { return false; }
    void mark_cleared_this_frame(bool) override {}
    bool is_swap_composite() const override { return false; }
    uint64_t swap_surface_id() const override { return 0; }

private:
    ::velk::IRenderBackend* backend_ = nullptr;
    ::VkImage       image_      = VK_NULL_HANDLE;
    ::VkImageView   view_       = VK_NULL_HANDLE;
    VmaAllocation   allocation_ = VK_NULL_HANDLE;
    uint32_t        bindless_index_ = 0;
    uint32_t        mip_levels_ = 1;
    ::VkImageLayout current_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    ::velk::uvec2       dimensions_{};
    ::velk::PixelFormat format_ = ::velk::PixelFormat::RGBA8;
    ::velk::SamplerDesc sampler_{};
};

/// Vulkan-backed renderable `IRenderTarget` + `IGpuTexture`
/// (TextureUsage::RenderTarget / ColorAttachment). Carries the
/// framebuffer + render passes for single-attachment RTT. MRT-group
/// attachments share the group's framebuffer / render pass and leave
/// these null.
class VkRenderTexture
    : public ::velk::ext::GpuResource<VkRenderTexture,
                                      ::velk::IRenderTarget,
                                      ::velk::IGpuTexture, IVkGpuTexture>
{
public:
    VELK_CLASS_UID(::velk::ClassId::VkRenderTexture, "VkRenderTexture");

    VkRenderTexture() = default;
    ~VkRenderTexture() override;

    // IGpuResource
    ::velk::GpuResourceType get_type() const override
    {
        return ::velk::GpuResourceType::Texture;
    }

    // ISurface (shared by IRenderTarget + IGpuTexture chains)
    ::velk::uvec2       get_dimensions()   const override { return dimensions_; }
    ::velk::PixelFormat format()           const override { return format_; }
    ::velk::SamplerDesc get_sampler_desc() const override { return sampler_; }

    // IRenderTarget
    ::velk::DepthFormat get_depth_format() const override { return depth_format_; }
    void set_depth_format(::velk::DepthFormat df) override { depth_format_ = df; }
    void set_size(uint32_t w, uint32_t h) override { dimensions_ = {w, h}; }
    void set_format(::velk::PixelFormat f) override { format_ = f; }

    // IVkGpuTexture
    void init_sampled(::velk::IRenderBackend*, ::VkImage, ::VkImageView,
                      VmaAllocation, uint32_t, uint32_t, ::VkImageLayout,
                      ::velk::uvec2, ::velk::PixelFormat,
                      const ::velk::SamplerDesc&) override
    { /* not supported on renderable impl */ }

    void init_render_target(::velk::IRenderBackend* backend,
                            ::VkImage image, ::VkImageView view,
                            VmaAllocation allocation,
                            uint32_t bindless_index, uint32_t mip_levels,
                            ::VkImageLayout initial_layout,
                            ::velk::uvec2 dimensions,
                            ::velk::PixelFormat format,
                            const ::velk::SamplerDesc& sampler,
                            ::VkFramebuffer framebuffer,
                            ::VkRenderPass render_pass,
                            ::VkRenderPass load_render_pass) override;

    ::VkImage       vk_image()           const override { return image_; }
    ::VkImageView   vk_view()            const override { return view_; }
    VmaAllocation   vk_allocation()      const override { return allocation_; }
    uint32_t        vk_bindless_index()  const override { return bindless_index_; }
    uint32_t        vk_mip_levels()      const override { return mip_levels_; }
    ::VkFramebuffer vk_framebuffer()      const override { return framebuffer_; }
    ::VkRenderPass  vk_render_pass()      const override { return render_pass_; }
    ::VkRenderPass  vk_load_render_pass() const override { return load_render_pass_; }
    ::VkImageLayout vk_current_layout()   const override { return current_layout_; }
    void set_vk_current_layout(::VkImageLayout l) override { current_layout_ = l; }
    bool was_cleared_this_frame() const override { return cleared_this_frame_; }
    void mark_cleared_this_frame(bool c) override { cleared_this_frame_ = c; }
    bool is_swap_composite() const override { return false; }
    uint64_t swap_surface_id() const override { return 0; }

private:
    ::velk::IRenderBackend* backend_ = nullptr;
    ::VkImage       image_       = VK_NULL_HANDLE;
    ::VkImageView   view_        = VK_NULL_HANDLE;
    VmaAllocation   allocation_  = VK_NULL_HANDLE;
    uint32_t        bindless_index_ = 0;
    uint32_t        mip_levels_  = 1;
    ::VkImageLayout current_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    ::VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    ::VkRenderPass  render_pass_ = VK_NULL_HANDLE;
    ::VkRenderPass  load_render_pass_ = VK_NULL_HANDLE;
    ::velk::uvec2       dimensions_{};
    ::velk::PixelFormat format_ = ::velk::PixelFormat::RGBA8;
    ::velk::SamplerDesc sampler_{};
    ::velk::DepthFormat depth_format_ = ::velk::DepthFormat::None;
    bool cleared_this_frame_ = false;
};

/// Sibling interface for backend code to call into VkSurfaceTexture
/// without a concrete-type cast (which would be ambiguous through the
/// IRenderTarget + IGpuTexture diamond on IInterface).
class IVkSurfaceTexture
    : public ::velk::Interface<IVkSurfaceTexture, ::velk::IInterface,
                               VELK_UID("0c91ad60-e0a4-4c87-8d61-30b6a3e85c4f")>
{
public:
    virtual void init(::velk::IRenderBackend* backend,
                      uint64_t surface_id,
                      ::VkImage image, ::VkImageView view,
                      VmaAllocation allocation,
                      ::velk::uvec2 dimensions,
                      ::velk::PixelFormat format) = 0;
    virtual void release(::VkDevice device, VmaAllocator allocator) = 0;
};

/// Per-surface composite intermediate.
///
/// Backed by a stable `VkImage` (recreated only on resize) so cached
/// secondary cmd buffers that reference its `vk_view()` work across
/// frames. The actual swapchain image rotation is hidden behind this
/// wrapper: producers render to the composite as if it were any
/// regular renderable+sampleable texture; the backend emits a final
/// composite-to-swap blit at `end_frame` for any surface whose
/// composite was rendered this frame.
///
/// Multi-view-to-same-surface is handled inside the recorder: first
/// `record_begin_rendering` of the frame on this composite respects
/// the producer's loadOp; subsequent records override loadOp=LOAD so
/// later views stack draws on top instead of overwriting. `record_blit_to_texture`
/// with this composite as dst marks it dirty so the final blit fires.
class VkSurfaceTexture
    : public ::velk::ext::GpuResource<VkSurfaceTexture,
                                      ::velk::IRenderTarget,
                                      ::velk::IGpuTexture,
                                      IVkGpuTexture,
                                      IVkSurfaceTexture>
{
public:
    VELK_CLASS_UID(::velk::ClassId::VkSurfaceTexture, "VkSurfaceTexture");

    VkSurfaceTexture() = default;
    ~VkSurfaceTexture() override;

    // IGpuResource
    ::velk::GpuResourceType get_type() const override
    {
        return ::velk::GpuResourceType::Texture;
    }

    // ISurface
    ::velk::uvec2       get_dimensions()   const override { return dimensions_; }
    ::velk::PixelFormat format()           const override { return format_; }
    ::velk::SamplerDesc get_sampler_desc() const override { return {}; }

    // IRenderTarget
    ::velk::DepthFormat get_depth_format() const override { return ::velk::DepthFormat::None; }
    void set_depth_format(::velk::DepthFormat) override {}
    void set_size(uint32_t w, uint32_t h) override { dimensions_ = {w, h}; }
    void set_format(::velk::PixelFormat f) override { format_ = f; }

    // IVkSurfaceTexture
    void init(::velk::IRenderBackend* backend,
              uint64_t surface_id,
              ::VkImage image, ::VkImageView view, VmaAllocation allocation,
              ::velk::uvec2 dimensions, ::velk::PixelFormat format) override;
    void release(::VkDevice device, VmaAllocator allocator) override;

    // IVkGpuTexture
    void init_sampled(::velk::IRenderBackend*, ::VkImage, ::VkImageView,
                      VmaAllocation, uint32_t, uint32_t, ::VkImageLayout,
                      ::velk::uvec2, ::velk::PixelFormat,
                      const ::velk::SamplerDesc&) override
    { /* not used; init() covers the surface-composite shape */ }

    void init_render_target(::velk::IRenderBackend*, ::VkImage, ::VkImageView,
                            VmaAllocation, uint32_t, uint32_t, ::VkImageLayout,
                            ::velk::uvec2, ::velk::PixelFormat,
                            const ::velk::SamplerDesc&, ::VkFramebuffer,
                            ::VkRenderPass, ::VkRenderPass) override
    { /* not used */ }

    ::VkImage      vk_image()           const override { return image_; }
    ::VkImageView  vk_view()            const override { return view_; }
    VmaAllocation  vk_allocation()      const override { return allocation_; }
    uint32_t       vk_bindless_index()  const override { return UINT32_MAX; }
    uint32_t       vk_mip_levels()      const override { return 1; }
    ::VkFramebuffer vk_framebuffer()      const override { return VK_NULL_HANDLE; }
    ::VkRenderPass  vk_render_pass()      const override { return VK_NULL_HANDLE; }
    ::VkRenderPass  vk_load_render_pass() const override { return VK_NULL_HANDLE; }
    ::VkImageLayout vk_current_layout()   const override { return current_layout_; }
    void set_vk_current_layout(::VkImageLayout l) override { current_layout_ = l; }
    bool was_cleared_this_frame() const override { return cleared_this_frame_; }
    void mark_cleared_this_frame(bool c) override { cleared_this_frame_ = c; }
    bool is_swap_composite() const override { return true; }
    uint64_t swap_surface_id() const override { return surface_id_; }

private:
    ::velk::IRenderBackend* backend_ = nullptr;
    uint64_t        surface_id_ = 0;
    ::VkImage       image_      = VK_NULL_HANDLE;
    ::VkImageView   view_       = VK_NULL_HANDLE;
    VmaAllocation   allocation_ = VK_NULL_HANDLE;
    ::VkImageLayout current_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    ::velk::uvec2       dimensions_{};
    ::velk::PixelFormat format_ = ::velk::PixelFormat::RGBA16F;
    bool cleared_this_frame_ = false;
};

} // namespace velk::vk

#endif // VELK_VK_GPU_TEXTURE_H
