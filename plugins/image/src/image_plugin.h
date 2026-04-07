#ifndef VELK_UI_IMAGE_PLUGIN_IMPL_H
#define VELK_UI_IMAGE_PLUGIN_IMPL_H

#include <velk/ext/plugin.h>

#include <velk-ui/plugins/image/plugin.h>

namespace velk::ui::impl {

class ImagePlugin final : public ::velk::ext::Plugin<ImagePlugin>
{
public:
    VELK_PLUGIN_UID(::velk::ui::PluginId::ImagePlugin);
    VELK_PLUGIN_NAME("velk_image");
    VELK_PLUGIN_VERSION(0, 1, 0);

    ReturnValue initialize(IVelk& velk, PluginConfig& config) override;
    ReturnValue shutdown(IVelk& velk) override;

private:
    IResourceDecoder::Ptr decoder_;
};

} // namespace velk::ui::impl

VELK_PLUGIN(velk::ui::impl::ImagePlugin)

#endif // VELK_UI_IMAGE_PLUGIN_IMPL_H
