#include "vk_command_buffer.h"

#include "vk_backend.h"

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

    // Per Vulkan, secondary command buffers don't inherit descriptor
    // bindings from the primary that vkCmdExecuteCommands them. Bind
    // the bindless graphics set here at the top of each secondary.
    //
    // The FrameGlobals BDA at push offset [0..8) is NOT pushed here:
    // it rotates per-frame, and a recorded secondary would freeze a
    // stale value into its bytecode and dereference a freed staging
    // address on subsequent frames. The render-graph executor pushes
    // it on the primary before each `vkCmdExecuteCommands`; the
    // secondary's commands see the primary's push state at execute
    // time provided the secondary doesn't itself touch offset [0..8).
    if (inherit_render_pass_ != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(cmd_,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                backend_->pipeline_layout_,
                                0, 1, &backend_->descriptor_set_,
                                0, nullptr);
    }
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

// S4.2.3a: dispatch / blit recordings are not used yet — producers
// continue to emit `ops::Dispatch` / `ops::BlitToSurface` ops which
// the executor routes through `VkBackend::dispatch` / `blit_to_surface`
// directly. These methods become real implementations when the
// compute / post-process producers migrate (S4.2.4 / S4.2.5).
void VkCommandBuffer::record_dispatch(const ::velk::DispatchCall& /*call*/) {}
void VkCommandBuffer::record_blit_to_surface(
    ::velk::TextureId /*source*/, uint64_t /*surface_id*/, ::velk::rect /*dst_rect*/)
{
}
void VkCommandBuffer::record_blit_group_depth_to_surface(
    ::velk::RenderTargetGroup /*src_group*/, uint64_t /*surface_id*/,
    ::velk::rect /*dst_rect*/)
{
}

} // namespace velk::vk
