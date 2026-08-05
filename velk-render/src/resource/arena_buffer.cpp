#include "arena_buffer.h"

namespace velk::impl {

void ArenaBuffer::init(IGpuArena* arena, uint64_t size)
{
    arena_ = arena;
    if (arena_ && size > 0) {
        region_ = arena_->alloc(size);
    }
}

bool ArenaBuffer::write(size_t sz, WriteFn fn, void* ctx)
{
    if (!arena_ || !fn || sz == 0) return false;

    // A size change takes a fresh region rather than reusing this one: the
    // deferred free is fenced but an in-place write is not, so a frame still
    // reading the old contents must keep them.
    if (region_.size() != static_cast<uint64_t>(sz)) {
        region_ = arena_->alloc(static_cast<uint64_t>(sz));
        if (!region_.valid()) return false;
    }

    void* dst = arena_->mapped_at(region_.offset());
    if (!dst) return false;
    fn(dst, sz, ctx);
    return true;
}

GpuRef ArenaBuffer::gpu_ref() const
{
    if (!arena_ || !region_.valid()) return {};
    const uint32_t stride = arena_->element_size();
    return GpuRef::from_index(arena_->slot(),
                              static_cast<uint32_t>(region_.offset() / (stride ? stride : 1u)));
}

} // namespace velk::impl
