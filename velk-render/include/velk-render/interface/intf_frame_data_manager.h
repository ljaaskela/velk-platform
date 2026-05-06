#ifndef VELK_RENDER_INTF_FRAME_DATA_MANAGER_H
#define VELK_RENDER_INTF_FRAME_DATA_MANAGER_H

#include <velk/interface/intf_interface.h>
#include <velk/uid.h>

#include <velk-render/interface/intf_render_backend.h>

#include <cstddef>
#include <cstdint>

namespace velk {

class IGpuResourceManager;

/**
 * @brief Per-frame GPU staging buffer.
 *
 * Path code uploads draw data (globals, instance records, material params)
 * via @c write / @c reserve into the active frame slot. The Renderer owns
 * one `IFrameDataManager` for the whole renderer plus one `Slot` per
 * in-flight frame; slot lifetime + grow logic live on the interface so
 * a plugin-provided implementation can swap the strategy without the
 * Renderer reaching for a concrete pointer.
 */
class IFrameDataManager
    : public Interface<IFrameDataManager, IInterface,
                       VELK_UID("e957caf0-d5d3-4abb-925b-0d12f63dcb30")>
{
public:
    /** @brief Persistently mapped GPU buffer for one in-flight frame. */
    struct Slot
    {
        GpuBufferHandle handle{};
        void* ptr = nullptr;
        uint64_t gpu_base = 0;
        size_t buffer_size = 0;
    };

    /** @brief Result of a raw write reservation. */
    struct WriteResult
    {
        void* ptr = nullptr;       ///< CPU pointer to write into, or nullptr on overflow.
        uint64_t gpu_addr = 0;     ///< GPU address of the reserved region.
    };

    static constexpr size_t kInitialSize = 1024 * 1024;

    /// @brief Sets the initial buffer size. Call before init_slot.
    virtual void init(size_t initial_size = kInitialSize) = 0;

    /// @brief Writes data to the active slot's buffer.
    /// @returns GPU address of the written region, or 0 on overflow.
    virtual uint64_t write(const void* data, size_t size, size_t alignment = 16) = 0;

    /// @brief Reserves space without writing. Caller fills the returned ptr.
    virtual WriteResult reserve(size_t size, size_t alignment = 16) = 0;

    /// @brief Allocates the initial GPU buffer for @p slot. @p resources
    ///        is used to defer destruction of any previous buffer
    ///        attached to @p slot through the resource manager's
    ///        completion-marker queue, so frame_data growth is safe even
    ///        if a prior frame's submit is still reading the old buffer.
    virtual void init_slot(Slot& slot,
                           IRenderBackend& backend,
                           IGpuResourceManager& resources) = 0;

    /// @brief Grows @p slot's buffer to the current target size if undersized.
    virtual void ensure_slot(Slot& slot,
                             IRenderBackend& backend,
                             IGpuResourceManager& resources) = 0;

    /// @brief Sets @p slot active for the upcoming frame and resets the write offset.
    virtual void begin_frame(Slot& slot) = 0;

    /// @brief Doubles target size if peak usage approaches the limit.
    virtual void ensure_capacity(IRenderBackend& backend,
                                 IGpuResourceManager& resources) = 0;

    /// @brief Grows the active slot's buffer in response to an overflow.
    virtual void grow(IRenderBackend& backend,
                      IGpuResourceManager& resources) = 0;

    /// @brief True iff the active frame has overflowed since begin_frame.
    virtual bool overflowed() const = 0;

    virtual size_t get_buffer_size() const = 0;
    virtual size_t get_peak_usage() const = 0;

    /// @brief Active slot's underlying GPU buffer. 0 if no slot is active.
    virtual GpuBufferHandle active_buffer() const = 0;

    /// @brief Active slot's GPU base address. Subtract from a written
    ///        gpu_addr to get a buffer offset for descriptor binding.
    virtual uint64_t active_buffer_base() const = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_FRAME_DATA_MANAGER_H
