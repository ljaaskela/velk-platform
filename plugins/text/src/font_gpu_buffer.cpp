#include "font_gpu_buffer.h"

namespace velk::ui {

void FontGpuBuffer::init(FontBuffers* fb, FontGpuBufferRole role)
{
    fb_ = fb;
    role_ = role;
}

size_t FontGpuBuffer::get_data_size() const
{
    if (fb_) {
        switch (role_) {
        case FontGpuBufferRole::Curves:
            return fb_->curves_bytes();
        case FontGpuBufferRole::Bands:
            return fb_->bands_bytes();
        case FontGpuBufferRole::Glyphs:
            return fb_->glyphs_bytes();
        }
    }
    return 0;
}

const uint8_t* FontGpuBuffer::get_data() const
{
    if (fb_) {
        switch (role_) {
        case FontGpuBufferRole::Curves:
            return reinterpret_cast<const uint8_t*>(fb_->curves());
        case FontGpuBufferRole::Bands:
            return reinterpret_cast<const uint8_t*>(fb_->bands());
        case FontGpuBufferRole::Glyphs:
            return reinterpret_cast<const uint8_t*>(fb_->glyphs());
        }
    }
    return nullptr;
}

bool FontGpuBuffer::is_dirty() const
{
    if (fb_) {
        switch (role_) {
        case FontGpuBufferRole::Curves:
            return fb_->curves_dirty();
        case FontGpuBufferRole::Bands:
            return fb_->bands_dirty();
        case FontGpuBufferRole::Glyphs:
            return fb_->glyphs_dirty();
        }
    }
    return false;
}

void FontGpuBuffer::clear_dirty()
{
    if (fb_) {
        switch (role_) {
        case FontGpuBufferRole::Curves:
            fb_->clear_curves_dirty();
            break;
        case FontGpuBufferRole::Bands:
            fb_->clear_bands_dirty();
            break;
        case FontGpuBufferRole::Glyphs:
            fb_->clear_glyphs_dirty();
            break;
        }
    }
}

} // namespace velk::ui
