#ifndef VELK_UI_TEXT_FONT_GPU_BUFFER_H
#define VELK_UI_TEXT_FONT_GPU_BUFFER_H

#include "font_buffers.h"

#include <velk/ext/object.h>

#include <velk-render/ext/gpu_resource.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-ui/plugins/text/plugin.h>

namespace velk::ui {

enum class FontGpuBufferRole
{
    Curves,
    Bands,
    Glyphs,
};

class IFontGpuBufferInternal : public Interface<IFontGpuBufferInternal>
{
public:
    virtual void init(FontBuffers* fb, FontGpuBufferRole role) = 0;
};

/**
 * @brief IBuffer wrapper around one section of a `FontBuffers` (curves,
 *        bands, or glyph table).
 *
 * The Font owns three of these, one per role. Each is a velk Object so it
 * can plug into the renderer's GPU resource lifecycle (observer, dirty
 * tracking, deferred destroy). The wrapper does not own the underlying
 * `FontBuffers` data; the Font does. The wrapper just exposes one section
 * of it as an `IBuffer` for the renderer to upload.
 *
 * The dirty flag is per-section: appending curves marks curves dirty
 * without forcing a re-upload of the bands or glyph table.
 *
 * The GPU virtual address is stored locally and set by the renderer after
 * each (re)allocation, then read by `TextMaterial::write_gpu_data` to emit
 * the buffer references the shader binds via `buffer_reference`.
 */
class FontGpuBuffer : public ::velk::ext::GpuResource<FontGpuBuffer, IBuffer, IFontGpuBufferInternal>
{
public:
    VELK_CLASS_UID(::velk::ui::ClassId::FontGpuBuffer, "FontGpuBuffer");

    GpuResourceType get_type() const override { return GpuResourceType::Buffer; }

    FontGpuBuffer() = default;

    // IFontGpuBufferInternal
    void init(FontBuffers* fb, FontGpuBufferRole role) override;

    // IBuffer
    size_t get_data_size() const override;
    const uint8_t* get_data() const override;
    bool is_dirty() const override;
    void clear_dirty() override;
    uint64_t get_gpu_address() const override { return gpu_addr_; }
    void set_gpu_address(uint64_t addr) override { gpu_addr_ = addr; }

private:
    FontBuffers* fb_ = nullptr;
    FontGpuBufferRole role_ = FontGpuBufferRole::Curves;
    uint64_t gpu_addr_ = 0;
};

} // namespace velk::ui

#endif // VELK_UI_TEXT_FONT_GPU_BUFFER_H
