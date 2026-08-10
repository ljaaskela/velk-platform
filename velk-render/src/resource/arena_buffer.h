#ifndef VELK_RENDER_ARENA_BUFFER_H
#define VELK_RENDER_ARENA_BUFFER_H

#include <velk/ext/object.h>

#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_gpu_arena.h>
#include <velk-render/plugin.h>

namespace velk::impl {

/// Construction-time hook, kept off IArenaBuffer so consumers see only the
/// read side.
class IArenaBufferInternal
    : public Interface<IArenaBufferInternal, IInterface,
                       VELK_UID("56774d3a-19e8-4d5e-af76-cc6d58efd37f")>
{
public:
    /// Binds this buffer to @p arena and reserves @p size bytes, keeping
    /// @p alignment across regrowth.
    virtual void init(IGpuArena* arena, uint64_t size, uint64_t alignment) = 0;
};

/**
 * @brief IBuffer backed by a region of an IGpuArena rather than by a GPU
 *        allocation of its own.
 *
 * Created by `IGpuArena::create_buffer`. The owner uses the ordinary IBuffer
 * write API and asks `get_gpu_ref` where the shader should index; it does not
 * deal with regions, offsets or bases itself.
 *
 * Write-only from the CPU side, because arena storage is allocated for
 * sequential writes and reads from it are slow. `write` fills the region in
 * place, so the bytes are copied once. `get_data` and `write_diff` are not
 * served: implementing only the half of IBuffer that the storage can actually
 * support is deliberate. Owners that need to diff their bytes should do so
 * against their own authoritative copy before writing.
 *
 * Growth reallocates: a `write` larger than the current region takes a fresh
 * one and drops the old, which is the required behaviour for data that changes
 * wholesale, and the old range retires on the fence.
 */
class ArenaBuffer
    : public ::velk::ext::ObjectCore<ArenaBuffer, IBuffer, IArenaBuffer,
                                     IArenaBufferInternal>
{
public:
    VELK_CLASS_UID(::velk::ClassId::ArenaBuffer, "ArenaBuffer");

    // IArenaBufferInternal
    void init(IGpuArena* arena, uint64_t size, uint64_t alignment) override;

    // IBuffer
    size_t get_data_size() const override { return static_cast<size_t>(region_.size()); }
    const uint8_t* get_data() const override { return nullptr; }
    bool is_dirty() const override { return false; }
    void clear_dirty() override {}
    bool write_diff(const void* /*bytes*/, size_t /*size*/) override { return false; }
    bool write(size_t sz, WriteFn fn, void* ctx) override;

    // IArenaBuffer
    GpuRef gpu_ref() const override;

private:
    /// Weak for the same reason ArenaRegion's reference is: an owner (a
    /// material, a font) can outlive the renderer that owns the arenas, and
    /// a write after the arena is gone must fail rather than call into it.
    IGpuArena::WeakPtr arena_;
    ArenaRegion region_;
    uint64_t alignment_ = 0;
};

} // namespace velk::impl

#endif // VELK_RENDER_ARENA_BUFFER_H
