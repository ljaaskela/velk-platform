#include <velk-render/ext/default_render_pass.h>

namespace velk::impl {

array_view<const IGpuResource::Ptr> DefaultRenderPass::reads() const
{
    return array_view<const IGpuResource::Ptr>(reads_.data(), reads_.size());
}

array_view<const IGpuResource::Ptr> DefaultRenderPass::writes() const
{
    return array_view<const IGpuResource::Ptr>(writes_.data(), writes_.size());
}

void DefaultRenderPass::add_read(IGpuResource::Ptr resource)
{
    reads_.push_back(std::move(resource));
}

void DefaultRenderPass::add_write(IGpuResource::Ptr resource)
{
    writes_.push_back(std::move(resource));
}

void DefaultRenderPass::reset()
{
    reads_.clear();
    writes_.clear();
    command_buffer_.reset();
}

} // namespace velk::impl
