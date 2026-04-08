#ifndef VELK_UI_TEXT_FONT_GPU_BUFFER_H
#define VELK_UI_TEXT_FONT_GPU_BUFFER_H

#include "font_buffers.h"

#include <velk/ext/object.h>

#include <velk-render/ext/gpu_resource.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-ui/plugins/text/plugin.h>

namespace velk::ui {

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
class FontGpuBuffer : public ::velk::ext::GpuResource<FontGpuBuffer, ::velk::IBuffer>
{
public:
    enum class Role
    {
        Curves,
        Bands,
        Glyphs,
    };

    VELK_CLASS_UID("e0d0f4f6-0c4b-4a8b-b7e4-7e2d6e1a0001", "FontGpuBuffer");

    FontGpuBuffer() = default;
    // Destructor is provided by ext::GpuResource (it notifies observers).

    /// Creates a wrapper attached to a FontBuffers section and returns it
    /// as an IBuffer::Ptr. Lifetime of `fb` must outlive the returned ptr.
    static ::velk::IBuffer::Ptr make(FontBuffers* fb, Role role);

    /// Bind the wrapper to a FontBuffers section. Called by `make`.
    void init(FontBuffers* fb, Role role)
    {
        fb_ = fb;
        role_ = role;
    }

    // IBuffer
    size_t get_size() const override
    {
        if (!fb_) {
            return 0;
        }
        switch (role_) {
        case Role::Curves: return fb_->curves_bytes();
        case Role::Bands:  return fb_->bands_bytes();
        case Role::Glyphs: return fb_->glyphs_bytes();
        }
        return 0;
    }

    const uint8_t* get_data() const override
    {
        if (!fb_) {
            return nullptr;
        }
        switch (role_) {
        case Role::Curves: return reinterpret_cast<const uint8_t*>(fb_->curves());
        case Role::Bands:  return reinterpret_cast<const uint8_t*>(fb_->bands());
        case Role::Glyphs: return reinterpret_cast<const uint8_t*>(fb_->glyphs());
        }
        return nullptr;
    }

    bool is_dirty() const override
    {
        if (!fb_) {
            return false;
        }
        switch (role_) {
        case Role::Curves: return fb_->curves_dirty();
        case Role::Bands:  return fb_->bands_dirty();
        case Role::Glyphs: return fb_->glyphs_dirty();
        }
        return false;
    }

    void clear_dirty() override
    {
        if (!fb_) {
            return;
        }
        switch (role_) {
        case Role::Curves: fb_->clear_curves_dirty(); return;
        case Role::Bands:  fb_->clear_bands_dirty();  return;
        case Role::Glyphs: fb_->clear_glyphs_dirty(); return;
        }
    }

    uint64_t get_gpu_address() const override { return gpu_addr_; }
    void set_gpu_address(uint64_t addr) override { gpu_addr_ = addr; }

private:
    FontBuffers* fb_ = nullptr;
    Role role_ = Role::Curves;
    uint64_t gpu_addr_ = 0;
};

} // namespace velk::ui

#endif // VELK_UI_TEXT_FONT_GPU_BUFFER_H
