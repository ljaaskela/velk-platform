#include "rect_visual.h"

#include <velk/api/state.h>

#include <cstring>

namespace velk_ui {

velk::vector<DrawEntry> RectVisual::get_draw_entries(const velk::rect& bounds)
{
    auto state = velk::read_state<IVisual>(this);
    if (!state) {
        return {};
    }

    DrawEntry entry{};
    entry.pipeline_key = PipelineKey::Rect;
    entry.bounds = bounds;

    float data[] = {bounds.x, bounds.y, bounds.width, bounds.height,
                    state->color.r, state->color.g, state->color.b, state->color.a};
    std::memcpy(entry.instance_data, data, sizeof(data));
    entry.instance_size = sizeof(data);

    return {entry};
}

} // namespace velk_ui
