#include <velk-render/ext/default_render_pass.h>

namespace velk::impl {

array_view<const GraphOp> DefaultRenderPass::ops() const
{
    return array_view<const GraphOp>(ops_.data(), ops_.size());
}

array_view<const IGpuResource::Ptr> DefaultRenderPass::reads() const
{
    return array_view<const IGpuResource::Ptr>(reads_.data(), reads_.size());
}

array_view<const IGpuResource::Ptr> DefaultRenderPass::writes() const
{
    return array_view<const IGpuResource::Ptr>(writes_.data(), writes_.size());
}

uint64_t DefaultRenderPass::view_globals_address() const
{
    return view_globals_address_;
}

void DefaultRenderPass::add_op(GraphOp op)
{
    ops_.push_back(std::move(op));
}

void DefaultRenderPass::add_read(IGpuResource::Ptr resource)
{
    reads_.push_back(std::move(resource));
}

void DefaultRenderPass::add_write(IGpuResource::Ptr resource)
{
    writes_.push_back(std::move(resource));
}

void DefaultRenderPass::set_view_globals_address(uint64_t addr)
{
    view_globals_address_ = addr;
}

} // namespace velk::impl
