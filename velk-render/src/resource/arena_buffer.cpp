#include "arena_buffer.h"

namespace velk::impl {

void ArenaBuffer::init(IGpuArena* arena, uint64_t size, uint64_t alignment)
{
    arena_ = ::velk::get_self<IGpuArena>(arena);
    alignment_ = alignment;
    if (arena && size > 0) {
        region_ = arena->alloc(size, alignment_);
    }
}

bool ArenaBuffer::write(size_t sz, WriteFn fn, void* ctx)
{
    auto arena = arena_.lock();
    if (!arena || !fn || sz == 0) return false;

    // A size change takes a fresh region rather than reusing this one: the
    // deferred free is fenced but an in-place write is not, so a frame still
    // reading the old contents must keep them.
    if (region_.size() != static_cast<uint64_t>(sz)) {
        region_ = arena->alloc(static_cast<uint64_t>(sz), alignment_);
        if (!region_.valid()) return false;
    }

    void* dst = arena->mapped_at(region_.offset());
    if (!dst) return false;
    fn(dst, sz, ctx);
    return true;
}

GpuRef ArenaBuffer::gpu_ref() const
{
    auto arena = arena_.lock();
    if (!arena || !region_.valid()) return {};
    const uint32_t stride = arena->element_size();
    return GpuRef::from_index(arena->slot(),
                              static_cast<uint32_t>(region_.offset() / (stride ? stride : 1u)));
}

} // namespace velk::impl
