#ifndef VELK_VK_COMMAND_BUFFER_H
#define VELK_VK_COMMAND_BUFFER_H

#include <velk/ext/object.h>

#include <velk-render/interface/intf_gpu_command_buffer.h>
#include <velk-render/platform.h>

#include <volk/volk.h>

namespace velk::vk {

class VkBackend;

/**
 * @brief Vulkan secondary-command-buffer backed `IGpuCommandBuffer`.
 *
 * Backend creates these via `VkBackend::create_command_buffer(target_id)`,
 * which initialises the impl with a backend reference + the
 * inheritance render pass derived from the target. Producer code
 * calls `begin_recording` / record / `end_recording` in the standard
 * IGpuCommandBuffer flow; the impl translates those into
 * `vkBeginCommandBuffer` / vkCmd* / `vkEndCommandBuffer` against a
 * VkCommandBuffer allocated from the backend's long-lived
 * secondary pool.
 *
 * S4.2.1 stub: no-op recording + execute. Real implementation
 * lands in S4.2.3 alongside the ForwardPath migration that drives
 * the secondary-cmd-buf flow end-to-end.
 */
class VkCommandBuffer : public ::velk::ext::ObjectCore<VkCommandBuffer, ::velk::IGpuCommandBuffer>
{
public:
    VELK_CLASS_UID(::velk::ClassId::VkCommandBuffer, "VkCommandBuffer");

    VkCommandBuffer() = default;
    ~VkCommandBuffer() override;

    /// Configures the cmd buffer with backend reference + the render
    /// pass it'll be recorded against (or `VK_NULL_HANDLE` for
    /// outside-renderpass cmd buffers).
    void init(VkBackend* backend, VkRenderPass inherit_render_pass);

    // IGpuCommandBuffer
    void begin_recording() override;
    void end_recording() override;
    void set_viewport(::velk::rect viewport) override;
void record_draws(::velk::array_view<const ::velk::DrawCall> calls) override;
    void record_dispatch(const ::velk::DispatchCall& call) override;
    void record_blit_to_surface(::velk::IGpuTexture& source,
                                uint64_t surface_id,
                                ::velk::rect dst_rect) override;
    void record_blit_to_texture(::velk::IGpuTexture& source,
                                ::velk::IGpuTexture& dest,
                                ::velk::rect dst_rect) override;
    void record_blit_group_depth_to_surface(
        ::velk::IRenderTextureGroup& src_group,
        uint64_t surface_id,
        ::velk::rect dst_rect) override;

    /// Backend access for the executor (`VkBackend::execute`).
    ::VkCommandBuffer handle() const { return cmd_; }

private:
    VkBackend* backend_ = nullptr;
    ::VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    ::VkRenderPass inherit_render_pass_ = VK_NULL_HANDLE;
};

} // namespace velk::vk

#endif // VELK_VK_COMMAND_BUFFER_H
