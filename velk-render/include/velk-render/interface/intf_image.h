#ifndef VELK_RENDER_INTF_IMAGE_H
#define VELK_RENDER_INTF_IMAGE_H

#include <velk/interface/resource/intf_resource.h>

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
 * flag inherited from `IResource`. It is intentionally minimal: width,
 * height, format, GPU handle and pixel data live on `ITexture`.
 *
 * The concrete `Image` class produced by the image plugin's decoder
 * implements **both** `IImage` and `ITexture`. Apps holding an
 * `IImage::Ptr` can `interface_cast<ITexture>(image)` to get the binding
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
};

} // namespace velk

#endif // VELK_RENDER_INTF_IMAGE_H
