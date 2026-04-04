#include "plugin.h"

#include "constraint/fixed_size.h"
#include "element.h"
#include "input/click.h"
#include "input/drag.h"
#include "input/hover.h"
#include "input/input_dispatcher.h"
#include "import/align_type_extension.h"
#include "import/dim_type_extension.h"
#include "layout/stack.h"
#include "material/gradient_material.h"
#include "material/shader_material.h"
#include "renderer/renderer.h"
#include "scene.h"
#include "transform/matrix.h"
#include "transform/trs.h"
#include "visual/rect_visual.h"
#include "visual/rounded_rect_visual.h"

#include <velk/ext/any.h>

namespace velk::ui {

ReturnValue VelkUiPlugin::initialize(IVelk& velk, PluginConfig& config)
{
    config.enableUpdate = true;

    auto rv = register_type<Element>(velk);
    rv &= register_type<Scene>(velk);
    rv &= register_type<Stack>(velk);
    rv &= register_type<FixedSize>(velk);
    rv &= register_type<RectVisual>(velk);
    rv &= register_type<RoundedRectVisual>(velk);
    rv &= register_type<ShaderMaterial>(velk);
    rv &= register_type<GradientMaterial>(velk);
    rv &= register_type<Renderer>(velk);
    rv &= register_type<Trs>(velk);
    rv &= register_type<Matrix>(velk);
    rv &= register_type<impl::InputDispatcher>(velk);
    rv &= register_type<Click>(velk);
    rv &= register_type<Hover>(velk);
    rv &= register_type<Drag>(velk);
    rv &= register_type<DimTypeExtension>(velk);
    rv &= register_type<AlignTypeExtension>(velk);
    rv &= register_type<::velk::ext::AnyValue<dim>>(velk);
    rv &= register_type<::velk::ext::AnyValue<HAlign>>(velk);
    rv &= register_type<::velk::ext::AnyValue<VAlign>>(velk);
    return rv;
}

ReturnValue VelkUiPlugin::shutdown(IVelk&)
{
    return ReturnValue::Success;
}

void VelkUiPlugin::post_update(const IPlugin::PostUpdateInfo& info)
{
    for (auto* scene : Scene::live_scenes()) {
        scene->update(info.info);
    }
}

} // namespace velk::ui
