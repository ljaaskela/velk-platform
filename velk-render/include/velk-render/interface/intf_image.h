#ifndef VELK_RENDER_INTF_IMAGE_H
#define VELK_RENDER_INTF_IMAGE_H

#include <velk/interface/resource/intf_resource.h>
#include <velk-render/render_types.h>

#include <cstdint>

namespace velk {

/// Lifecycle status of an `IImage`.
enum class ImageStatus : uint8_t
{
    Unloaded, ///< Resource exists but no decode has been attempted yet.
    Loading,  ///< Decode is in progress (future async loading).
    Loaded,   ///< Decoded successfully; the texture surface is valid.
    Failed,   ///< Decode was attempted and failed.
};

/**
 * @brief A 2D image resource identified by URI, with a load lifecycle.
 *
 * `IImage` is the resource concept: an image known by URI, cached and
 * deduplicated by the resource store, optionally pinned via the persistence
 * flag inherited from `IResource`. It is intentionally minimal: dimensions
 * and format live on `ISurface`, pixel data on `IBuffer`.
 *
 * The concrete `Image` class produced by the image plugin's decoder
 * implements `IImage`, `ISurface`, and `IBuffer`. Apps holding an
 * `IImage::Ptr` can `interface_cast<ISurface>(image)` to get the binding
 * surface for materials.
 *
 * Sync v1 only ever produces `Loaded` or `Failed` status. `Unloaded` and
 * `Loading` exist in the enum so consumers can be written defensively for
 * future lazy and async loading without an interface change.
 *
 * Chain: IInterface -> IResource -> IImage
 */
class IImage : public Interface<IImage, IResource>
{
public:
    /** @brief Returns the current load status. */
    virtual ImageStatus status() const = 0;

    /**
     * @brief Overrides the sampler desc used when this image is uploaded.
     *
     * Loaders that know more than the image plugin (e.g. the glTF
     * importer reading a `sampler` record) call this between decode and
     * first observation by the renderer. Has no effect once the texture
     * has been uploaded — the sampler captured at upload time is final.
     */
    virtual void set_sampler_desc(const SamplerDesc& desc) = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_IMAGE_H
