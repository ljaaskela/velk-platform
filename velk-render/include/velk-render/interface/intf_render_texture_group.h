#ifndef VELK_RENDER_INTF_RENDER_TEXTURE_GROUP_H
#define VELK_RENDER_INTF_RENDER_TEXTURE_GROUP_H

#include <velk/uid.h>

#include <velk-render/interface/intf_render_target.h>
#include <velk-render/render_types.h>

namespace velk {

/**
 * @brief A multi-attachment renderable target.
 *
 * Wraps a backend `RenderTargetGroup` (the bind handle for `begin_pass`)
 * plus its sampleable attachment `TextureId`s. Used by deferred paths
 * for G-buffer fill: GBufferFill writes the group; the lighting pass
 * reads each attachment via `IGpuResource::get_gpu_handle(N)` keyed by
 * attachment index.
 *
 * Lifecycle: backend allocates the group + attachments via
 * `create_render_target_group`. The owner stores the group id via
 * `set_render_target_id` and caches the attachment ids via
 * `set_attachment`. On removal the owner calls
 * `backend->destroy_render_target_group(group->get_gpu_handle(GpuResourceKey::Default))`.
 *
 * Chain: IInterface -> IGpuResource -> ISurface -> IRenderTarget -> IRenderTextureGroup
 */
class IRenderTextureGroup
    : public Interface<IRenderTextureGroup, IRenderTarget,
                       VELK_UID("5ea6d99b-2404-4f72-84e0-626b7c2d5343")>
{
public:
    /// Number of cached attachments. Convenience for owners / debug code.
    virtual size_t attachment_count() const = 0;

    /// Releases all cached attachment ids.
    virtual void clear_attachments() = 0;

    /// Backing IGpuTexture for attachment @p index. Returns nullptr if
    /// the wrapper hasn't been populated.
    virtual class IGpuTexture* attachment_texture(uint32_t index) const = 0;

    /// Returns the bindless `TextureId` of attachment @p index. Derived
    /// from `attachment_texture(i)`'s default gpu_handle.
    TextureId attachment(uint32_t index) const
    {
        return get_texture_id(attachment_texture(index));
    }
};

} // namespace velk

#endif // VELK_RENDER_INTF_RENDER_TEXTURE_GROUP_H
