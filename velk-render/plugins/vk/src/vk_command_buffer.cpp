#include "vk_command_buffer.h"

#include "vk_backend.h"
#include "vk_gpu_texture.h"

#include <velk-render/interface/intf_gpu_texture.h>

#include <velk/api/velk.h>

#include <cstring>

namespace velk::vk {

#ifdef VELK_RENDER_DEBUG
#define RENDER_LOG(...) VELK_LOG(I, __VA_ARGS__)
#else
#define RENDER_LOG(...) ((void)0)
#endif

VkCommandBuffer::~VkCommandBuffer()
{
    if (cmd_ != VK_NULL_HANDLE && backend_ != nullptr) {
        // The cmd buffer may still be in flight on the GPU when the
        // producer drops its Ptr (cache.dirty rebuild swaps Ptrs).
        // Defer the free to the next time the current frame slot rolls
        // around — by then kFrameOverlap subsequent frames will have
        // completed and any prior submission referencing this buffer
        // is guaranteed done.
        backend_->defer_free_persistent_secondary(cmd_);
        cmd_ = VK_NULL_HANDLE;
    }
}

void VkCommandBuffer::init(VkBackend* backend)
{
    backend_ = backend;
}

void VkCommandBuffer::begin_recording()
{
    if (!backend_) return;
    if (cmd_ == VK_NULL_HANDLE) {
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = backend_->persistent_secondary_pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        ai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(backend_->device_, &ai, &cmd_) != VK_SUCCESS) {
            cmd_ = VK_NULL_HANDLE;
            return;
        }
    }
    RENDER_LOG("vk.cmdbuf.begin_recording this=%p cb=%p", (void*)this, (void*)cmd_);

    // Pool created with RESET_COMMAND_BUFFER_BIT, so vkBeginCommandBuffer
    // implicitly resets the buffer on a re-begin.
    //
    // S6.6: secondaries are self-contained dynamic-rendering — they
    // call vkCmdBeginRendering / vkCmdEndRendering inside
    // record_begin_rendering / record_end_rendering. No inheritance
    // render pass; no RENDER_PASS_CONTINUE_BIT.
    VkCommandBufferInheritanceInfo inh{};
    inh.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;

    // SIMULTANEOUS_USE_BIT: the same persistent secondary can be
    // referenced by primaries from up to kFrameOverlap concurrent
    // in-flight frames. Without this flag the cmd buffer must finish
    // executing before being re-submitted, which our frame pacing
    // doesn't guarantee.
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
    bi.pInheritanceInfo = &inh;
    vkBeginCommandBuffer(cmd_, &bi);

    // Secondary command buffers inherit no state from the primary;
    // bind the bindless descriptor set. COMPUTE bind initially —
    // record_begin_rendering rebinds GRAPHICS for raster passes.
    vkCmdBindDescriptorSets(cmd_,
                            VK_PIPELINE_BIND_POINT_COMPUTE,
                            backend_->pipeline_layout_,
                            0, 1, &backend_->descriptor_set_,
                            0, nullptr);
}

void VkCommandBuffer::end_recording()
{
    if (cmd_ == VK_NULL_HANDLE) return;
    vkEndCommandBuffer(cmd_);
}

void VkCommandBuffer::set_viewport(::velk::rect viewport)
{
    if (cmd_ == VK_NULL_HANDLE || !backend_) return;
    float vp_w = viewport.width;
    float vp_h = viewport.height;

    VkViewport vp{};
    vp.x = viewport.x;
    vp.y = viewport.y;
    vp.width = vp_w;
    vp.height = vp_h;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd_, 0, 1, &vp);

    VkRect2D scissor{};
    scissor.offset = {static_cast<int32_t>(viewport.x), static_cast<int32_t>(viewport.y)};
    scissor.extent = {static_cast<uint32_t>(vp_w), static_cast<uint32_t>(vp_h)};
    vkCmdSetScissor(cmd_, 0, 1, &scissor);
}

void VkCommandBuffer::record_draws(::velk::array_view<const ::velk::DrawCall> calls)
{
    if (cmd_ == VK_NULL_HANDLE || !backend_) return;
    RENDER_LOG("vk.cmdbuf.record_draws this=%p cb=%p calls=%zu",
               (void*)this, (void*)cmd_, calls.size());
    backend_->record_draw_loop(cmd_, calls);
}

void VkCommandBuffer::record_dispatch(const ::velk::DispatchCall& call)
{
    if (cmd_ == VK_NULL_HANDLE || !backend_) return;
    RENDER_LOG("vk.cmdbuf.record_dispatch this=%p cb=%p pipeline=%llu",
               (void*)this, (void*)cmd_, (unsigned long long)call.pipeline);
    backend_->record_dispatch_call(cmd_, call);

    // Standard compute->fragment barrier so subsequent samples of the
    // dispatch's storage-image writes see the result. Mirrors the
    // legacy `VkBackend::dispatch` trailing barrier; baked into the
    // secondary so cached executes carry their own sync.
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd_,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);
}

void VkCommandBuffer::record_blit_to_texture(
    ::velk::IGpuTexture& source, ::velk::IGpuTexture& dest, ::velk::rect dst_rect)
{
    if (cmd_ == VK_NULL_HANDLE || !backend_) return;
    RENDER_LOG("vk.cmdbuf.record_blit_to_texture this=%p cb=%p src=%u dst=%u",
               (void*)this, (void*)cmd_,
               ::velk::get_texture_id(&source), ::velk::get_texture_id(&dest));
    backend_->record_blit_to_texture(cmd_, source, dest, dst_rect);
}

namespace {

/// Bake one image-layout transition into @p cb. Updates the texture's
/// tracked layout. The barrier is permissive on access masks (memory
/// barrier is for safety; downstream visibility comes from the
/// pre-pass barrier the graph executor emits before this cmd buffer).
void transition_image(::VkCommandBuffer cb,
                      IVkGpuTexture* tex,
                      ::VkImageLayout new_layout,
                      ::VkImageAspectFlags aspect)
{
    const ::VkImageLayout old_layout = tex->vk_current_layout();
    if (old_layout == new_layout) return;

    ::VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = tex->vk_image();
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.levelCount = tex->vk_mip_levels();
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
                    | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                    | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
                    | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                    | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                    | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                    | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
    tex->set_vk_current_layout(new_layout);
}

} // namespace

void VkCommandBuffer::record_begin_rendering(
    ::velk::array_view<const ::velk::ColorAttachment> colors,
    const ::velk::DepthAttachment* depth)
{
    if (cmd_ == VK_NULL_HANDLE || !backend_) return;
    RENDER_LOG("vk.cmdbuf.record_begin_rendering this=%p cb=%p colors=%zu depth=%d",
               (void*)this, (void*)cmd_, colors.size(), depth ? 1 : 0);

    // Resolve render area from the first attachment's dimensions.
    // All attachments must share the same dimensions per Vulkan spec.
    ::velk::uvec2 dims{};
    if (!colors.empty() && colors[0].texture) {
        dims = colors[0].texture->get_dimensions();
    } else if (depth && depth->texture) {
        dims = depth->texture->get_dimensions();
    }
    if (dims.x == 0 || dims.y == 0) return;

    // Layout transitions: each attachment goes to *_ATTACHMENT_OPTIMAL
    // before vkCmdBeginRendering. Per-texture vk_current_layout tracks
    // the assumed layout — first record from UNDEFINED is safe; later
    // re-records pick up the SHADER_READ_ONLY layout left by a prior
    // record_end_rendering. The cached barrier values capture
    // record-time layout, so the cmd buffer is replay-correct as long
    // as inter-frame state matches what was assumed at record time.
    for (const auto& c : colors) {
        if (!c.texture) continue;
        auto* vk_t = interface_cast<IVkGpuTexture>(c.texture);
        if (!vk_t) continue;
        transition_image(cmd_, vk_t,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_ASPECT_COLOR_BIT);
    }
    if (depth && depth->texture) {
        auto* vk_t = interface_cast<IVkGpuTexture>(depth->texture);
        if (vk_t) {
            transition_image(cmd_, vk_t,
                             VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_ASPECT_DEPTH_BIT);
        }
    }

    // Build VkRenderingAttachmentInfo for each color + optional depth.
    // Capacity matches DeferredPath's gbuffer (4 colors); raise if
    // future producers need more.
    constexpr size_t kMaxColors = 8;
    ::VkRenderingAttachmentInfo color_infos[kMaxColors]{};
    const size_t n_colors = (colors.size() < kMaxColors) ? colors.size() : kMaxColors;
    for (size_t i = 0; i < n_colors; ++i) {
        const auto& c = colors[i];
        auto& info = color_infos[i];
        info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ::velk::vk::IVkGpuTexture* vk_t = nullptr;
        if (c.texture) {
            vk_t = interface_cast<IVkGpuTexture>(c.texture);
            info.imageView = vk_t ? vk_t->vk_view() : VK_NULL_HANDLE;
        }
        // Multi-view-to-same-surface (S6.4): the per-surface composite
        // is shared across views. Backend.begin_frame clears the
        // composite each frame, so producer record_begin_rendering on
        // the composite is silently overridden to LOAD — views stack
        // on top of each other (and on top of the begin_frame clear)
        // instead of erasing prior content.
        bool effective_clear = c.clear;
        if (vk_t && vk_t->is_swap_composite()) {
            effective_clear = false;
        }
        info.loadOp = effective_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        info.clearValue.color.float32[0] = c.clear_color[0];
        info.clearValue.color.float32[1] = c.clear_color[1];
        info.clearValue.color.float32[2] = c.clear_color[2];
        info.clearValue.color.float32[3] = c.clear_color[3];
    }

    ::VkRenderingAttachmentInfo depth_info{};
    if (depth && depth->texture) {
        depth_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        auto* vk_t = interface_cast<IVkGpuTexture>(depth->texture);
        depth_info.imageView = vk_t ? vk_t->vk_view() : VK_NULL_HANDLE;
        depth_info.loadOp = depth->clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depth_info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth_info.clearValue.depthStencil.depth = depth->clear_depth;
        depth_info.clearValue.depthStencil.stencil = depth->clear_stencil;
    }

    ::VkRenderingInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering_info.renderArea.extent = {dims.x, dims.y};
    rendering_info.layerCount = 1;
    rendering_info.colorAttachmentCount = static_cast<uint32_t>(n_colors);
    rendering_info.pColorAttachments = (n_colors > 0) ? color_infos : nullptr;
    rendering_info.pDepthAttachment = (depth && depth->texture) ? &depth_info : nullptr;

    vkCmdBeginRendering(cmd_, &rendering_info);

    // Record-time bookkeeping for the matching record_end_rendering.
    // Re-bind the bindless graphics descriptor here — secondary inherit
    // info had no render pass, so begin_recording bound COMPUTE; switch
    // to GRAPHICS for the upcoming draws.
    vkCmdBindDescriptorSets(cmd_,
                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                            backend_->pipeline_layout_,
                            0, 1, &backend_->descriptor_set_,
                            0, nullptr);

    rendering_color_count_ = static_cast<uint32_t>(n_colors);
    for (size_t i = 0; i < n_colors; ++i) {
        rendering_color_textures_[i] = colors[i].texture;
    }
    rendering_depth_texture_ = (depth && depth->texture) ? depth->texture : nullptr;
}

void VkCommandBuffer::record_end_rendering()
{
    if (cmd_ == VK_NULL_HANDLE || !backend_) return;
    RENDER_LOG("vk.cmdbuf.record_end_rendering this=%p cb=%p", (void*)this, (void*)cmd_);

    vkCmdEndRendering(cmd_);

    // Transition color attachments to SHADER_READ_ONLY so subsequent
    // samples (post-process effects, deferred-lighting bindless reads)
    // can pick them up without an extra graph-emitted barrier. Matches
    // the finalLayout convention from the legacy render-pass machinery.
    for (uint32_t i = 0; i < rendering_color_count_; ++i) {
        auto* tex = rendering_color_textures_[i];
        if (!tex) continue;
        auto* vk_t = interface_cast<IVkGpuTexture>(tex);
        if (!vk_t) continue;
        transition_image(cmd_, vk_t,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_ASPECT_COLOR_BIT);
    }
    // Depth attachments are created without VK_IMAGE_USAGE_SAMPLED_BIT
    // (the gbuffer's depth isn't sampled directly — worldpos lives in a
    // dedicated color attachment), so SHADER_READ_ONLY would be invalid.
    // Leave depth in DEPTH_ATTACHMENT_OPTIMAL; the next frame's
    // record_begin_rendering picks it up as a no-op transition.

    rendering_color_count_ = 0;
    rendering_depth_texture_ = nullptr;
}

} // namespace velk::vk
