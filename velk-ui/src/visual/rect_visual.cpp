#include "rect_visual.h"

#include <velk/api/state.h>
#include <velk/hash.h>
#include <velk-ui/instance_types.h>

namespace velk::ui {

vector<DrawEntry> RectVisual::get_draw_entries(const ::velk::size& bounds)
{
    auto state = read_state<IVisual>(this);
    if (!state) {
        return {};
    }

    DrawEntry entry{};
    entry.pipeline_key = get_raster_pipeline_key();
    entry.bounds = {0, 0, bounds.width, bounds.height};
    entry.set_instance(ElementInstance{
        {},  // world_matrix: written by batch_builder per-instance
        {0.f, 0.f, 0.f, 0.f},
        {bounds.width, bounds.height, 0.f, 0.f},
        state->color,
        {0u, 0u, 0u, 0u}});

    return {entry};
}

} // namespace velk::ui
