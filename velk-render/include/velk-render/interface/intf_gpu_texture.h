#ifndef VELK_RENDER_INTF_GPU_TEXTURE_H
#define VELK_RENDER_INTF_GPU_TEXTURE_H

#include <velk-render/interface/intf_surface.h>

namespace velk {

/**
 * @brief A sampleable GPU image with managed lifetime.
 *
 * Returned by `IRenderBackend::create_texture` / the resource manager
 * factories. Producers hold `IGpuTexture::Ptr` (owning) or
 * `IGpuTexture*` (non-owning view kept alive by the cache / wrapper
 * that owns the Ptr). Dropping the last Ptr defers destruction
 * through the backend's frame-completion-marker queue.
 *
 * The shader-side identifier (the uint32 materials embed in
 * `DrawDataHeader.texture_id`) lives in
 * `IGpuResource::get_gpu_handle(GpuResourceKey::Default)`. Free helper
 * `get_texture_id(p)` does the cast.
 *
 * Renderable textures additionally implement `IRenderTarget` (which
 * also derives from `ISurface`), forming a diamond on `ISurface`
 * resolved via the framework's `get_interface<>` lookup.
 *
 * Chain: IInterface -> IGpuResource -> ISurface -> IGpuTexture
 */
class IGpuTexture
    : public Interface<IGpuTexture, ISurface,
                       VELK_UID("394e18a0-a343-4589-bc9b-901cf3942a07")>
{
    // Marker interface: the shader-side id flows through
    // IGpuResource::get_gpu_handle(Default); pixel metadata flows
    // through ISurface (get_dimensions / format / get_sampler_desc).
};

/// Shader-side id for a texture (the uint32 materials embed in
/// `DrawDataHeader.texture_id`). 0 if @p ptr has no GPU storage.
template <typename T>
inline uint32_t get_texture_id(const T& ptr)
{
    auto* gt = interface_cast<IGpuTexture>(ptr);
    return gt ? static_cast<uint32_t>(gt->get_gpu_handle(GpuResourceKey::Default)) : 0;
}

} // namespace velk

#endif // VELK_RENDER_INTF_GPU_TEXTURE_H
