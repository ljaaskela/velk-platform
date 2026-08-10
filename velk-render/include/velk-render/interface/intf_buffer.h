#ifndef VELK_RENDER_INTF_BUFFER_H
#define VELK_RENDER_INTF_BUFFER_H

#include <cstddef>
#include <cstdint>
#include <velk/api/velk.h>
#include <velk-render/interface/intf_gpu_buffer.h>

namespace velk {

/**
 * @brief A chunk of CPU-resident bytes that may be uploaded somewhere
 *        (a GPU buffer, a GPU texture, or nowhere at all).
 *
 * Pure CPU byte source: size, dirty tracking, write API. No GPU
 * coupling — concrete implementations that also own a GPU buffer
 * compose `IGpuBuffer` + `IGpuBufferStorageOwner` alongside `IBuffer`;
 * texture-byte sources (Image, Environment) compose `ISurface`;
 * pure CPU buffers implement just this.
 *
 * Two lifecycles fit the same interface:
 *
 * - **Dynamic source** (e.g. font glyph buffers that grow lazily):
 *   keeps `get_data()` valid, sets `is_dirty()` after mutation, and
 *   the renderer re-uploads on the next frame.
 * - **Static source** (e.g. a decoded png): `get_data()` returns the
 *   bytes once, `is_dirty()` returns true on first observation, the
 *   renderer uploads, the buffer clears its dirty flag and may free
 *   its CPU bytes.
 */
class IBuffer : public Interface<IBuffer, IInterface>
{
public:
    /** @brief Returns the size of the CPU-resident byte block, in bytes. */
    virtual size_t get_data_size() const = 0;

    /**
     * @brief Returns the CPU-resident bytes, or nullptr if not available
     *        (e.g. a static resource whose CPU bytes have been freed after
     *        upload, or a GPU-only resource).
     */
    virtual const uint8_t* get_data() const = 0;

    /**
     * @brief Returns true if `get_data()` content (or size) has changed
     *        since the last upload and the renderer should re-upload on the
     *        next frame.
     */
    virtual bool is_dirty() const = 0;

    /**
     * @brief Called by the renderer after uploading from `get_data()`.
     *        Implementations may free their CPU byte buffer here if they no
     *        longer need it (e.g. one-shot static images).
     */
    virtual void clear_dirty() = 0;

    /**
     * @brief Memcmp-gated write of @p size bytes from @p bytes into the
     *        buffer's CPU-resident store. Returns true if the bytes (or
     *        size) actually differ from the current contents and the
     *        buffer is now dirty; false on no-op. Lets callers gate
     *        side effects (uploads, observer notifications) on real
     *        change.
     *
     *        Specialised IBuffer implementations whose bytes come from
     *        a private ingest path (texture pixels, font glyph atlas,
     *        program-data callbacks) are not required to support this
     *        and may return false unconditionally.
     */
    virtual bool write_diff(const void* bytes, size_t size) = 0;

    /**
     * @brief C-style writer callback. Receives a writable destination
     *        of @p sz bytes and the caller's @p ctx. Used by
     *        `IBuffer::write` to let callers fill the buffer in place
     *        without an intermediate scratch.
     */
    using WriteFn = void (*)(void* dst, size_t sz, void* ctx);

    /**
     * @brief Asks the buffer to (re)serialise @p sz bytes via @p fn.
     *
     * The buffer supplies the destination pointer (its own internal
     * scratch zeroed to @p sz), calls @p fn to fill it, then memcmps
     * against the previously committed bytes. Returns true if the
     * committed content changed and the buffer is now dirty; false if
     * identical (no re-upload needed).
     *
     * The point vs `write_diff`: callers that build the blob from
     * multiple sources (offsets / ranges) can write straight into the
     * buffer's own storage, avoiding the intermediate vector + memcpy
     * pair.
     *
     * Implementations that don't support arbitrary writes (texture
     * pixels, etc.) may return false unconditionally.
     */
    virtual bool write(size_t sz, WriteFn fn, void* ctx) = 0;

    /**
     * @brief Lambda-friendly overload of `write` that forwards to the
     *        virtual through a trampoline.
     */
    template <typename Writer>
    bool write(size_t sz, Writer&& writer)
    {
        auto trampoline = [](void* dst, size_t n, void* ctx) {
            (*static_cast<Writer*>(ctx))(dst, n);
        };
        return this->write(sz, trampoline, const_cast<Writer*>(&writer));
    }

};

/**
 * @brief Where a shader finds a buffer's contents.
 *
 * Replaces a bare `uint64_t` address, which said nothing about what the number
 * meant and so allowed an element base to travel under an address-shaped name.
 * No shader dereferences a GPU pointer any more, so the only way a shader
 * reaches a buffer is by indexing a bound `set = 1` arena; `Kind` is kept as a
 * named seam because a second backend may introduce another way.
 *
 * Construct only through the factories, so `kind` can never disagree with the
 * payload. A default-constructed ref is `None`, meaning "no GPU storage yet".
 * Note that a buffer bound wholesale (indirect args, an index buffer) has no
 * shader-visible handle at all and so answers `None`.
 */
struct GpuRef
{
    enum class Kind : uint32_t
    {
        None,   ///< No storage yet, or nothing a shader can index.
        Index,  ///< Region of a bound set = 1 arena; the shader indexes it.
    };

    Kind kind = Kind::None;
    struct { uint32_t base; uint32_t slot; } index;

    constexpr GpuRef() : kind(Kind::None), index{0, 0} {}

    static GpuRef from_index(uint32_t slot, uint32_t base)
    {
        GpuRef r;
        r.kind = Kind::Index;
        r.index.slot = slot;
        r.index.base = base;
        return r;
    }

    bool valid() const { return kind != Kind::None; }

    /// Element base within its arena. Base 0 is a valid value and cannot
    /// double as a sentinel, so reading a ref that holds no index is reported
    /// rather than defaulted.
    uint32_t get_base() const
    {
        if (kind != Kind::Index) {
            VELK_LOG(E, "GpuRef: read as arena index, but holds nothing");
            return 0;
        }
        return index.base;
    }

    /// The set = 1 binding this ref indexes. Not needed by shaders (the
    /// binding is declared statically); consumers use it to check a ref came
    /// from the slot they read.
    uint32_t get_slot() const
    {
        if (kind != Kind::Index) {
            VELK_LOG(E, "GpuRef: slot read, but the ref holds no arena index");
            return 0;
        }
        return index.slot;
    }
};

/**
 * @brief Implemented by buffers whose storage is a region of an IGpuArena,
 *        so consumers can ask where the shader should index.
 *
 * This is what `get_gpu_ref` looks for, and the only source of a valid ref.
 *
 * Chain: IInterface -> IArenaBuffer
 */
class IArenaBuffer
    : public Interface<IArenaBuffer, IInterface,
                       VELK_UID("8e4723bb-dc6a-4b1e-a0eb-b069836ab2df")>
{
public:
    /// Kind::Index once storage exists, Kind::None before.
    virtual GpuRef gpu_ref() const = 0;
};

/**
 * @brief Convenience: returns where a shader indexes @p ptr's contents, or an
 *        invalid ref when there is nowhere yet.
 *
 * Doubles as the residency test for arena-backed data: `valid()` is false
 * until the bytes have been suballocated. A buffer that is not arena-backed
 * answers `None`, since a shader has no way to reach it by hand.
 */
template <typename T>
GpuRef get_gpu_ref(const T& ptr)
{
    if (auto* ab = interface_cast<IArenaBuffer>(ptr)) {
        return ab->gpu_ref();
    }
    return {};
}

} // namespace velk

#endif // VELK_RENDER_INTF_BUFFER_H
