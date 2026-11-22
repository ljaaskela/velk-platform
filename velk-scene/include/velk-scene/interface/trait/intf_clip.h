#ifndef VELK_SCENE_INTF_CLIP_H
#define VELK_SCENE_INTF_CLIP_H

#include <velk/api/math_types.h>
#include <velk/interface/intf_metadata.h>

namespace velk {

/**
 * @brief Clips the element and its descendants to the element's box.
 *
 * What lies outside is not drawn, to the pixel, and does not take pointer input. Nested clips
 * intersect. The clip is an axis-aligned rectangle in world x and y, for 2D content; ray
 * traced shadows and reflections ignore it.
 */
class IClip : public Interface<IClip>
{
public:
    VELK_INTERFACE(
        (PROP, bool, enabled, true) ///< False draws everything, as without the trait.
    )

    /**
     * @brief The part of the element's box to clip to, in the element's space.
     * @param box The element's size.
     * @return Usually the whole box; a scroll view leaves out its scroll bars.
     */
    virtual rect clip_rect(const size& box) const = 0;
};

} // namespace velk

#endif // VELK_SCENE_INTF_CLIP_H
