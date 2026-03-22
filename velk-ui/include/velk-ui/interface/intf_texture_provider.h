#ifndef VELK_UI_INTF_TEXTURE_PROVIDER_H
#define VELK_UI_INTF_TEXTURE_PROVIDER_H

#include <velk/interface/intf_interface.h>

#include <cstdint>

namespace velk_ui {

/**
 * @brief Provides pixel data for a texture.
 *
 * Visuals that need textures (e.g. TextVisual with a glyph atlas) implement
 * this alongside IVisual. The renderer checks for it, uploads pixels to a GPU
 * texture, and caches the handle.
 */
class ITextureProvider : public velk::Interface<ITextureProvider>
{
public:
    /** @brief Returns pointer to pixel data (single-channel alpha). */
    virtual const uint8_t* get_pixels() const = 0;

    /** @brief Texture width in pixels. */
    virtual uint32_t get_texture_width() const = 0;

    /** @brief Texture height in pixels. */
    virtual uint32_t get_texture_height() const = 0;

    /** @brief True if pixel data has changed since last clear_texture_dirty(). */
    virtual bool is_texture_dirty() const = 0;

    /** @brief Marks the texture as clean after the renderer uploads it. */
    virtual void clear_texture_dirty() = 0;
};

} // namespace velk_ui

#endif // VELK_UI_INTF_TEXTURE_PROVIDER_H
