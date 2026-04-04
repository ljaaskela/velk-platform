#ifndef VELK_RENDER_INTF_TEXTURE_PROVIDER_H
#define VELK_RENDER_INTF_TEXTURE_PROVIDER_H

#include <velk/interface/intf_interface.h>

#include <cstdint>

namespace velk {

/**
 * @brief Provides pixel data for a texture.
 *
 * Visuals that need textures (e.g. TextVisual with a glyph atlas) implement
 * this alongside IVisual. The renderer checks for it, uploads pixels to a GPU
 * texture, and caches the handle.
 */
class ITextureProvider : public Interface<ITextureProvider>
{
public:
    virtual const uint8_t* get_pixels() const = 0;
    virtual uint32_t get_texture_width() const = 0;
    virtual uint32_t get_texture_height() const = 0;
    virtual bool is_texture_dirty() const = 0;
    virtual void clear_texture_dirty() = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_TEXTURE_PROVIDER_H
