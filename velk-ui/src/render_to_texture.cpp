#include "render_to_texture.h"

#include <velk/api/object_ref.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/interface/intf_metadata_observer.h>
#include <velk-render/plugin.h>

namespace velk::ui::impl {

RenderCache::RenderCache() = default;

void RenderCache::on_state_changed(string_view name, IMetadata&, Uid interfaceId)
{
    if (interfaceId != IRenderToTexture::UID) {
        return;
    }
}

} // namespace velk::ui::impl
