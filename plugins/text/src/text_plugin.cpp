#include "text_plugin.h"

#include "font.h"
#include "visual/text_material.h"
#include "visual/text_visual.h"

namespace velk::ui {

ReturnValue TextPlugin::initialize(IVelk& velk, PluginConfig&)
{
    auto rv = register_type<impl::Font>(velk);
    rv &= register_type<TextMaterial>(velk);
    rv &= register_type<TextVisual>(velk);

    // Create shared scale-independent default font
    default_font_ = ::velk::instance().create<IFont>(ClassId::Font);
    if (default_font_) {
        default_font_->init_default();
    }

    return rv;
}

ReturnValue TextPlugin::shutdown(IVelk&)
{
    default_font_.reset();
    return ReturnValue::Success;
}

IFont::Ptr TextPlugin::get_default_font() const
{
    return default_font_;
}

} // namespace velk::ui
