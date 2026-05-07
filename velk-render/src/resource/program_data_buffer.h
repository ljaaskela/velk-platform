#ifndef VELK_RENDER_PROGRAM_DATA_BUFFER_H
#define VELK_RENDER_PROGRAM_DATA_BUFFER_H

#include <velk/vector.h>

#include <velk-render/ext/gpu_resource.h>
#include <velk-render/interface/intf_program_data_buffer.h>
#include <velk-render/plugin.h>

namespace velk::impl {

/**
 * @brief Concrete IProgramDataBuffer implementation backing materials'
 *        per-draw persistent data. See intf_program_data_buffer.h for
 *        the contract. Holds both a committed byte vector (published
 *        via IBuffer::get_data) and a reused scratch used during
 *        `write()` diffs.
 */
class ProgramDataBuffer
    : public ::velk::ext::GpuResource<ProgramDataBuffer, IProgramDataBuffer>
{
public:
    VELK_CLASS_UID(::velk::ClassId::ProgramDataBuffer, "ProgramDataBuffer");

    ProgramDataBuffer() = default;

    GpuResourceType get_type() const override { return GpuResourceType::Buffer; }

    // IProgramDataBuffer
    bool write(size_t sz, WriteFn fn, void* ctx) override;

    // IBuffer
    size_t get_data_size() const override { return bytes_.size(); }
    const uint8_t* get_data() const override
    {
        return bytes_.empty() ? nullptr : bytes_.data();
    }
    bool is_dirty() const override { return dirty_; }
    void clear_dirty() override { dirty_ = false; }
    bool write_diff(const void* /*bytes*/, size_t /*size*/) override { return false; }

private:
    ::velk::vector<uint8_t> bytes_;    ///< Committed content visible to consumers.
    ::velk::vector<uint8_t> pending_;  ///< Reused write scratch, diffed vs bytes_.
    bool dirty_ = false;
};

} // namespace velk::impl

#endif // VELK_RENDER_PROGRAM_DATA_BUFFER_H
