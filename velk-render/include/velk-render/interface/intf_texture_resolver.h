#ifndef VELK_RENDER_INTF_TEXTURE_RESOLVER_H
#define VELK_RENDER_INTF_TEXTURE_RESOLVER_H

#include <velk-render/interface/intf_render_backend.h>

namespace velk {

class ISurface;

/**
 * @brief Resolves an ISurface to its bindless TextureId.
 *
 * Passed into IDrawData::write_draw_data / get_data_buffer so materials
 * that sample textures can embed TextureIds directly into their UBO.
 */
class ITextureResolver
{
public:
    virtual ~ITextureResolver() = default;

    /// Returns the bindless TextureId for @p surface, or 0 if none is
    /// available (e.g. not uploaded yet, or resolver null).
    virtual TextureId resolve(ISurface* surface) const = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_TEXTURE_RESOLVER_H
