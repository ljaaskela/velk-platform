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

    void add_read(IGpuResource::Ptr resource) override;
    void add_write(IGpuResource::Ptr resource) override;
    void set_view_globals_address(uint64_t addr) override;
    void set_command_buffer(IGpuCommandBuffer::Ptr cmd) override
    {
        command_buffer_ = std::move(cmd);
    }
    void set_held_pipelines(vector<IGpuPipeline::Ptr> pipelines) override
    {
        held_pipelines_ = std::move(pipelines);
    }
    void reset() override;

private:
    vector<IGpuResource::Ptr> reads_;
    vector<IGpuResource::Ptr> writes_;
    uint64_t view_globals_address_ = 0;
    IGpuCommandBuffer::Ptr command_buffer_;
    /// Strong refs to the pipelines this pass's command buffer binds;
    /// the pipeline cache holds only weak refs, so this keeps them alive
    /// for as long as the recorded pass is live. See `set_held_pipelines`.
    vector<IGpuPipeline::Ptr> held_pipelines_;
};

} // namespace velk::impl

#endif // VELK_RENDER_EXT_DEFAULT_RENDER_PASS_H
