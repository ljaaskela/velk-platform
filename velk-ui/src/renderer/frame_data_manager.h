#ifndef VELK_UI_FRAME_DATA_MANAGER_H
#define VELK_UI_FRAME_DATA_MANAGER_H

#include <velk-render/interface/intf_render_backend.h>

#include <cstdint>

namespace velk::ui {

/**
 * @brief Manages per-frame GPU data buffers.
 *
 * Each frame slot has a persistently-mapped GPU buffer that the renderer
 * writes draw data into (globals, instance data, material params).
 * The manager handles allocation, growth, and overflow detection.
 */
class FrameDataManager
{
public:
    struct Slot
    {
        GpuBuffer handle{};
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

    /** @brief Sets the initial buffer size. Call before init_slot. */
    void init(size_t initial_size = kInitialSize) { buffer_size_ = initial_size; }

    /** @brief Writes data to the active slot's buffer. Returns GPU address, or 0 on overflow. */
    uint64_t write(const void* data, size_t size, size_t alignment = 16);

    /** @brief Reserves space without writing. Returns raw pointer + GPU address. */
    WriteResult reserve(size_t size, size_t alignment = 16);

    /** @brief Resets write offset for a new frame. */
    void begin_frame(Slot& slot);

    /** @brief Checks if the buffer should grow based on peak usage. */
    void ensure_capacity(IRenderBackend& backend);

    /** @brief Grows the active slot's buffer after an overflow. */
    void grow(IRenderBackend& backend);

    /** @brief Allocates the initial buffer for a slot. */
    void init_slot(Slot& slot, IRenderBackend& backend);

    /** @brief Ensures a slot's buffer matches the current target size. Reallocates if undersized. */
    void ensure_slot(Slot& slot, IRenderBackend& backend);

    /** @brief Returns true if an overflow occurred during this frame. */
    bool overflowed() const { return overflow_; }

    /** @brief Returns the current buffer size in bytes. */
    size_t get_buffer_size() const { return buffer_size_; }

    /** @brief Returns the peak usage in bytes. */
    size_t get_peak_usage() const { return peak_usage_; }

private:
    void alloc_slot(Slot& slot, IRenderBackend& backend);

    size_t buffer_size_ = 0;
    size_t write_offset_ = 0;
    size_t peak_usage_ = 0;
    bool overflow_ = false;
    Slot* active_ = nullptr;
};

} // namespace velk::ui

#endif // VELK_UI_FRAME_DATA_MANAGER_H
