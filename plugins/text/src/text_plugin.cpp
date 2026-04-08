#include "text_plugin.h"

#include "font.h"
#include "font_gpu_buffer.h"
#include "visual/text_material.h"
#include "visual/text_visual.h"

namespace velk::ui {

ReturnValue TextPlugin::initialize(IVelk& velk, PluginConfig&)
{
    auto rv = register_type<Font>(velk);
    rv &= register_type<FontGpuBuffer>(velk);
    rv &= register_type<TextMaterial>(velk);
    rv &= register_type<TextVisual>(velk);

    // Create shared default font. Font is scale-independent now: any text
    // visual using it can render at any pixel size by setting its own
    // font_size property.
    auto obj = ::velk::instance().create<IObject>(ClassId::Font);
    default_font_ = interface_pointer_cast<IFont>(obj);
    if (default_font_) {
        default_font_->init_default();
    }

    return rv;
}

ReturnValue TextPlugin::shutdown(IVelk&)
{
    default_font_ = nullptr;
    return ReturnValue::Success;
}

IFont::Ptr TextPlugin::default_font() const
{
    return default_font_;
}

} // namespace velk::ui
