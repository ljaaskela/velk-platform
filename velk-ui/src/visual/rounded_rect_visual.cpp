#include "rounded_rect_visual.h"

#include <velk/api/state.h>
#include <velk-ui/instance_types.h>

namespace velk::ui {

vector<DrawEntry> RoundedRectVisual::get_draw_entries(const rect& bounds)
{
    auto state = read_state<IVisual>(this);
    if (!state) {
        return {};
    }

    DrawEntry entry{};
    entry.pipeline_key = PipelineKey::RoundedRect;
    entry.bounds = bounds;
    entry.set_instance(RectInstance{
        {bounds.x, bounds.y},
        {bounds.width, bounds.height},
        state->color});

    return {entry};
}

} // namespace velk::ui
