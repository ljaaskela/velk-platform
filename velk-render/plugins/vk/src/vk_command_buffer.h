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
 * Backend creates these via `VkBackend::create_command_buffer(target)`,
 * which initialises the impl with a backend reference + the
 * inheritance render pass derived from the target. Producer code
 * calls `begin_recording` / record / `end_recording` in the standard
 * IGpuCommandBuffer flow; the impl translates those into
 * `vkBeginCommandBuffer` / vkCmd* / `vkEndCommandBuffer` against a
 * VkCommandBuffer allocated from the backend's long-lived
 * secondary pool.
 */
class VkCommandBuffer : public ::velk::ext::ObjectCore<VkCommandBuffer, ::velk::IGpuCommandBuffer>
{
public:
    VELK_CLASS_UID(::velk::ClassId::VkCommandBuffer, "VkCommandBuffer");

    VkCommandBuffer() = default;
    ~VkCommandBuffer() override;

    /// Configures the cmd buffer with backend reference.
    /// All cmd buffers are self-contained dynamic-rendering secondaries
    /// (S6); no inheritance render pass needed.
    void init(VkBackend* backend);

    // IGpuCommandBuffer
    void begin_recording() override;
    void end_recording() override;
    void push_label(const char* name) override;
    void pop_label() override;
    void set_viewport(::velk::rect viewport) override;
void record_draws(::velk::array_view<const ::velk::DrawCall> calls) override;
    void record_dispatch(const ::velk::DispatchCall& call) override;
    void record_blit_to_texture(::velk::IGpuTexture& source,
                                ::velk::IGpuTexture& dest,
                                ::velk::rect dst_rect) override;
    void record_begin_rendering(
        ::velk::array_view<const ::velk::ColorAttachment> colors,
        const ::velk::DepthAttachment* depth) override;
    void record_end_rendering() override;

    /// Backend access for the executor (`VkBackend::execute`).
    ::VkCommandBuffer handle() const { return cmd_; }

private:
    VkBackend* backend_ = nullptr;
    ::VkCommandBuffer cmd_ = VK_NULL_HANDLE;

    /// Attachment textures captured between `record_begin_rendering`
    /// and `record_end_rendering` so the matching end-side layout
    /// transitions can run without re-walking the input list. Capacity
    /// matches the begin side; raise if future producers need more.
    static constexpr uint32_t kMaxRenderingColors = 8;
    ::velk::IGpuTexture* rendering_color_textures_[kMaxRenderingColors]{};
    ::velk::IGpuTexture* rendering_depth_texture_ = nullptr;
    uint32_t rendering_color_count_ = 0;
};

} // namespace velk::vk

#endif // VELK_VK_COMMAND_BUFFER_H
