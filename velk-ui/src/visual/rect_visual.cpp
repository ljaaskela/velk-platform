#include "rect_visual.h"

#include <velk/api/state.h>
#include <velk/hash.h>
#include <velk-ui/instance_types.h>

namespace velk::ui {

vector<DrawEntry> RectVisual::get_draw_entries(const rect& bounds)
{
    auto state = read_state<IVisual>(this);
    if (!state) {
        return {};
    }

    DrawEntry entry{};
    entry.pipeline_key = get_raster_pipeline_key();
    entry.bounds = bounds;
    entry.set_instance(RectInstance{
        {},  // world_matrix: written by batch_builder per-instance
        {bounds.x, bounds.y},
        {bounds.width, bounds.height},
        state->color});

    return {entry};
}

} // namespace velk::ui
