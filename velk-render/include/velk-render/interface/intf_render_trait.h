#ifndef VELK_RENDER_INTF_RENDER_TRAIT_H
#define VELK_RENDER_INTF_RENDER_TRAIT_H

#include <velk/interface/intf_metadata.h>

namespace velk {

/**
 * @brief Marker base for renderer-enumerated scene content attached
 *        to a scene element.
 *
 * Peer category to `velk::ITrait` (which participates in the UI
 * layout pipeline). `IRenderTrait` attachments aren't run by the
 * layout solver — the renderer queries them each frame via
 * `interface_cast<IRenderTrait>` (and then casts to the specific
 * kind: `ICamera`, `ILight`, `IVisual`, etc.) to collect scene
 * content for rendering.
 *
 * Render traits host `IRenderTechnique` attachments (via the
 * `RenderTrait` API wrapper's `add_technique`). Example: a Light's
 * shadow technique, a Camera's reflection technique.
 *
 * The interface itself has no methods — it's a semantic marker. The
 * specific contract lives in the sub-interface.
 */
class IRenderTrait : public Interface<IRenderTrait>
{
};

} // namespace velk

#endif // VELK_RENDER_INTF_RENDER_TRAIT_H
