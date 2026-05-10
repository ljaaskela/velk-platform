#ifndef VELK_RENDER_EXT_DEFAULT_RENDER_PASS_H
#define VELK_RENDER_EXT_DEFAULT_RENDER_PASS_H

#include <velk/vector.h>

#include <velk-render/ext/render_state.h>
#include <velk-render/interface/intf_render_pass.h>
#include <velk-render/plugin.h>

namespace velk::impl {

/**
 * @brief Default `IRenderPass` implementation: a renderer-facing data
 *        record that pipelines fill once per frame and add to the
 *        render graph.
 *
 * Hive-pooled via `velk::instance().create<IRenderPass>(ClassId::DefaultRenderPass)`.
 * Producers use the IRenderPass mutator surface (`add_read`, `add_write`,
 * `set_command_buffer`, `set_target_*`, `set_surface_blit`,
 * `set_view_globals_address`); they don't reach for this concrete
 * type so plugins outside velk-render can build passes through the
 * interface alone.
 */
class DefaultRenderPass : public ::velk::ext::RenderState<DefaultRenderPass, IRenderPass>
{
public:
    VELK_CLASS_UID(ClassId::DefaultRenderPass, "DefaultRenderPass");

    array_view<const IGpuResource::Ptr> reads() const override;
    array_view<const IGpuResource::Ptr> writes() const override;
    uint64_t view_globals_address() const override;
    IGpuCommandBuffer::Ptr command_buffer() const override { return command_buffer_; }
    uint64_t target_id() const override { return target_id_; }
    IGpuTexture* target_texture() const override { return target_texture_; }
    IRenderTextureGroup* target_group() const override { return target_group_; }
    IGpuTexture* surface_blit_source() const override { return surface_blit_source_; }
    uint64_t surface_blit_surface_id() const override { return surface_blit_surface_id_; }
    rect surface_blit_rect() const override { return surface_blit_rect_; }

    void add_read(IGpuResource::Ptr resource) override;
    void add_write(IGpuResource::Ptr resource) override;
    void set_view_globals_address(uint64_t addr) override;
    void set_command_buffer(IGpuCommandBuffer::Ptr cmd) override
    {
        command_buffer_ = std::move(cmd);
    }
    void set_target_id(uint64_t target_id) override { target_id_ = target_id; }
    void set_target_texture(IGpuTexture* texture) override { target_texture_ = texture; }
    void set_target_group(IRenderTextureGroup* group) override { target_group_ = group; }
    void set_surface_blit(IGpuTexture* source,
                          uint64_t surface_id,
                          rect dst_rect) override
    {
        surface_blit_source_ = source;
        surface_blit_surface_id_ = surface_id;
        surface_blit_rect_ = dst_rect;
    }
    void reset() override;

private:
    vector<IGpuResource::Ptr> reads_;
    vector<IGpuResource::Ptr> writes_;
    uint64_t view_globals_address_ = 0;
    IGpuCommandBuffer::Ptr command_buffer_;
    uint64_t target_id_ = 0;
    IGpuTexture* target_texture_ = nullptr;
    IRenderTextureGroup* target_group_ = nullptr;
    IGpuTexture* surface_blit_source_ = nullptr;
    uint64_t surface_blit_surface_id_ = 0;
    rect surface_blit_rect_{};
};

} // namespace velk::impl

#endif // VELK_RENDER_EXT_DEFAULT_RENDER_PASS_H
