#include "vk_command_buffer.h"

#include "vk_backend.h"

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

void VkCommandBuffer::init(VkBackend* backend, ::VkRenderPass inherit_render_pass)
{
    backend_ = backend;
    inherit_render_pass_ = inherit_render_pass;
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
    RENDER_LOG("vk.cmdbuf.begin_recording this=%p cb=%p inherit_rp=%p",
               (void*)this, (void*)cmd_, (void*)inherit_render_pass_);

    // Pool created with RESET_COMMAND_BUFFER_BIT, so vkBeginCommandBuffer
    // implicitly resets the buffer on a re-begin.
    VkCommandBufferInheritanceInfo inh{};
    inh.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
    inh.renderPass = inherit_render_pass_;
    inh.subpass = 0;
    inh.framebuffer = VK_NULL_HANDLE;

    // SIMULTANEOUS_USE_BIT: the same persistent secondary can be
    // referenced by primaries from up to kFrameOverlap concurrent
    // in-flight frames. Without this flag the cmd buffer must finish
    // executing before being re-submitted, which our frame pacing
    // doesn't guarantee.
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT
        | ((inherit_render_pass_ != VK_NULL_HANDLE)
            ? VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT
            : 0u);
    bi.pInheritanceInfo = &inh;
    vkBeginCommandBuffer(cmd_, &bi);

    // Secondary command buffers inherit no state from the primary.
    // Bind the bindless descriptor set for the buffer's bind point.
    // Per-view FrameGlobals are reached via DrawData.globals_addr
    // (see render_architecture_cleanup.md S5.3) — stable BDA, no
    // explicit push-constant.
    const VkPipelineBindPoint bind_point = (inherit_render_pass_ != VK_NULL_HANDLE)
        ? VK_PIPELINE_BIND_POINT_GRAPHICS
        : VK_PIPELINE_BIND_POINT_COMPUTE;
    vkCmdBindDescriptorSets(cmd_,
                            bind_point,
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
    float vp_w = (viewport.width > 0)
        ? viewport.width
        : static_cast<float>(backend_->current_target_width_);
    float vp_h = (viewport.height > 0)
        ? viewport.height
        : static_cast<float>(backend_->current_target_height_);

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

} // namespace velk::vk
