#ifndef VELK_RENDER_EXT_DEFAULT_RENDER_PASS_H
#define VELK_RENDER_EXT_DEFAULT_RENDER_PASS_H

#include <velk/ext/core_object.h>
#include <velk/vector.h>

#include <velk-render/interface/intf_render_pass.h>
#include <velk-render/plugin.h>

namespace velk::impl {

/**
 * @brief Default `IRenderPass` implementation: a renderer-facing data
 *        record that pipelines fill once per frame and add to the
 *        render graph.
 *
 * Hive-pooled via `velk::instance().create<IRenderPass>(ClassId::DefaultRenderPass)`.
 * Producers use the IRenderPass mutator surface (`add_op`, `add_read`,
 * `add_write`, `set_view_globals`); they don't reach for this concrete
 * type so plugins outside velk-render can build passes through the
 * interface alone.
 */
class DefaultRenderPass : public ext::ObjectCore<DefaultRenderPass, IRenderPass>
{
public:
    VELK_CLASS_UID(ClassId::DefaultRenderPass, "DefaultRenderPass");

    array_view<const GraphOp> ops() const override
    {
        return array_view<const GraphOp>(ops_.data(), ops_.size());
    }
    array_view<const IGpuResource::Ptr> reads() const override
    {
        return array_view<const IGpuResource::Ptr>(reads_.data(), reads_.size());
    }
    array_view<const IGpuResource::Ptr> writes() const override
    {
        return array_view<const IGpuResource::Ptr>(writes_.data(), writes_.size());
    }

    GpuBuffer view_globals_buffer() const override { return view_globals_buffer_; }
    uint64_t  view_globals_offset() const override { return view_globals_offset_; }
    uint32_t  view_globals_range()  const override { return view_globals_range_;  }

    void add_op(GraphOp op) override { ops_.push_back(std::move(op)); }
    void add_read(IGpuResource::Ptr resource) override
    {
        reads_.push_back(std::move(resource));
    }
    void add_write(IGpuResource::Ptr resource) override
    {
        writes_.push_back(std::move(resource));
    }
    void set_view_globals(GpuBuffer buffer, uint64_t offset, uint32_t range) override
    {
        view_globals_buffer_ = buffer;
        view_globals_offset_ = offset;
        view_globals_range_  = range;
    }

private:
    vector<GraphOp> ops_;
    vector<IGpuResource::Ptr> reads_;
    vector<IGpuResource::Ptr> writes_;
    GpuBuffer view_globals_buffer_ = 0;
    uint64_t  view_globals_offset_ = 0;
    uint32_t  view_globals_range_  = 0;
};

} // namespace velk::impl

#endif // VELK_RENDER_EXT_DEFAULT_RENDER_PASS_H
