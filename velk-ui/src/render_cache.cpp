#include "render_cache.h"

#include <velk/interface/intf_metadata_observer.h>

namespace velk::ui::impl {

void RenderCache::on_state_changed(string_view name, IMetadata&, Uid interfaceId)
{
    constexpr string_view names[] = {"texture_size", "render_target"};
    if (has_state_changed<IRenderToTexture>(interfaceId, name, names)) {
        invoke_trait_dirty(DirtyFlags::Visual);
    }
}

} // namespace velk::ui::impl
