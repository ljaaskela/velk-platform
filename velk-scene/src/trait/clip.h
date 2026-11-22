#ifndef VELK_SCENE_CLIP_H
#define VELK_SCENE_CLIP_H

#include <velk-scene/ext/trait.h>
#include <velk-scene/interface/trait/intf_clip.h>
#include <velk-scene/plugin.h>

namespace velk::impl {

/** @brief See IClip. Read by the batch builder; takes no part in layout. */
class Clip : public ::velk::ext::Trait<Clip, ITrait, IClip>
{
public:
    VELK_CLASS_UID(ClassId::Clip, "Clip");

    TraitPhase get_phase() const override { return TraitPhase::None; }
    rect clip_rect(const size& box) const override { return {0.f, 0.f, box.width, box.height}; }

protected:
    void on_state_changed(string_view, IMetadata&, Uid) override { invoke_trait_dirty(DirtyFlags::Visual); }
};

} // namespace velk::impl

#endif // VELK_SCENE_CLIP_H
