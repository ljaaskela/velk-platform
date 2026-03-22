#include "font_atlas.h"

#include <algorithm>
#include <cstring>

namespace velk_ui {

GlyphAtlas::GlyphAtlas(uint32_t width, uint32_t height) : width_(width), height_(height)
{
    pixels_.resize(width * height, 0);
}

const AtlasRect* GlyphAtlas::ensure_glyph(IFont& font, uint32_t glyph_id)
{
    auto it = glyphs_.find(glyph_id);
    if (it != glyphs_.end()) {
        return &it->second;
    }

    IFont::GlyphBitmap bmp = font.rasterize_glyph(glyph_id);
    if (!bmp.data || bmp.width == 0 || bmp.height == 0) {
        // Insert a zero-size entry for whitespace glyphs
        AtlasRect rect{};
        rect.bearing_x = bmp.bearing.x;
        rect.bearing_y = bmp.bearing.y;
        auto result = glyphs_.emplace(glyph_id, rect);
        return &result.first->second;
    }

    uint32_t gw = bmp.width + 1; // 1px padding
    uint32_t gh = bmp.height + 1;

    // Advance to next row if needed
    if (cursor_x_ + gw > width_) {
        cursor_x_ = 0;
        cursor_y_ += row_height_;
        row_height_ = 0;
    }

    // Out of space
    if (cursor_y_ + gh > height_) {
        return nullptr;
    }

    // Copy glyph bitmap into atlas
    for (uint32_t row = 0; row < bmp.height; ++row) {
        uint32_t dst_offset = (cursor_y_ + row) * width_ + cursor_x_;
        std::memcpy(&pixels_[dst_offset], bmp.data + row * bmp.width, bmp.width);
    }

    AtlasRect rect;
    rect.x = cursor_x_;
    rect.y = cursor_y_;
    rect.w = bmp.width;
    rect.h = bmp.height;
    rect.bearing_x = bmp.bearing.x;
    rect.bearing_y = bmp.bearing.y;

    cursor_x_ += gw;
    row_height_ = std::max(row_height_, gh);
    dirty_ = true;

    auto result = glyphs_.emplace(glyph_id, rect);
    return &result.first->second;
}

void GlyphAtlas::clear()
{
    std::memset(pixels_.data(), 0, pixels_.size());
    glyphs_.clear();
    cursor_x_ = 0;
    cursor_y_ = 0;
    row_height_ = 0;
    dirty_ = true;
}

} // namespace velk_ui
