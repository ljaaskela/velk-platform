#include "plugin.h"

#include <velk/interface/intf_plugin_registry.h>
#include <velk-render/interface/intf_camera.h>
#include <velk-render/interface/intf_light.h>
#include <velk-scene/interface/intf_scene_plugin.h>
#include <velk-scene/plugin.h>

#include "constraint/fixed_size.h"
#include "import/align_type_extension.h"
#include "import/dim_type_extension.h"
#include "import/light_type_extension.h"
#include "import/projection_type_extension.h"
#include "import/visual_phase_type_extension.h"
#include "input/click.h"
#include "input/drag.h"
#include "input/hover.h"
#include "input/input_dispatcher.h"
#include "layout/stack.h"
#include "material/gradient_material.h"
#include "layout_solver.h"
#include "visual/rect_visual.h"
#include "visual/rounded_rect_visual.h"
#include "render_cache.h"
#include "visual/texture_material.h"
#include "visual/texture_visual.h"

#include <velk/ext/any.h>

namespace velk::ui {

ReturnValue VelkUiPlugin::initialize(IVelk& velk, PluginConfig& config)
{
    config.enableUpdate = true;

    auto rv = register_type<Stack>(velk);
    rv &= register_type<FixedSize>(velk);
    rv &= register_type<RectVisual>(velk);
    rv &= register_type<RoundedRectVisual>(velk);
    rv &= register_type<impl::TextureVisual>(velk);
    rv &= register_type<impl::TextureMaterial>(velk);
    rv &= register_type<impl::RenderCache>(velk);
    rv &= register_type<GradientMaterial>(velk);
    rv &= register_type<impl::InputDispatcher>(velk);
    rv &= register_type<Click>(velk);
    rv &= register_type<Hover>(velk);
    rv &= register_type<Drag>(velk);
    // Low instance count types, alloc as needed.
    ::velk::TypeOptions alloc;
    alloc.policy = ::velk::CreationPolicy::Alloc;
    rv &= register_type<DimTypeExtension>(velk, alloc);
    rv &= register_type<AlignTypeExtension>(velk, alloc);
    rv &= register_type<ProjectionTypeExtension>(velk, alloc);
    rv &= register_type<VisualPhaseTypeExtension>(velk, alloc);
    rv &= register_type<LightTypeExtension>(velk, alloc);
    // Value type registrations
    rv &= register_type<::velk::ext::AnyValue<dim>>(velk);
    rv &= register_type<::velk::ext::AnyValue<DirtyFlags>>(velk);
    rv &= register_type<::velk::ext::AnyValue<HAlign>>(velk);
    rv &= register_type<::velk::ext::AnyValue<VAlign>>(velk);
    rv &= register_type<::velk::ext::AnyValue<Projection>>(velk);
    rv &= register_type<::velk::ext::AnyValue<VisualPhase>>(velk);
    rv &= register_type<::velk::ext::AnyValue<LightType>>(velk);
    rv &= register_type<::velk::ext::AnyValue<PointerEvent>>(velk);
    rv &= register_type<::velk::ext::AnyValue<ScrollEvent>>(velk);
    rv &= register_type<::velk::ext::AnyValue<KeyEvent>>(velk);
    rv &= register_type<::velk::ext::AnyValue<DragEvent>>(velk);
    return rv;
}

ReturnValue VelkUiPlugin::shutdown(IVelk&)
{
    return ReturnValue::Success;
}

void VelkUiPlugin::post_update(const IPlugin::PostUpdateInfo& info)
{
    // Pre-update: run velk-ui's LayoutSolver against every live scene
    // before its own update() runs. The solver is the velk-ui side of
    // Scene::update — it materialises the constraints + transforms +
    // AABB propagation that scene-model itself doesn't know about.
    // Scene::update afterwards merges resulting dirty notifications
    // and finalises per-frame state.
    auto plugin_ptr = ::velk::instance().plugin_registry().find_plugin(
        ::velk::PluginId::ScenePlugin);
    auto* scene_svc = interface_cast<IScenePlugin>(plugin_ptr.get());
    if (!scene_svc) return;

    LayoutSolver solver;
    struct Ctx { LayoutSolver* solver; const IPlugin::PostUpdateInfo* info; };
    Ctx ctx{&solver, &info};
    scene_svc->for_each_scene(+[](IScene* scene, void* user) {
        if (!scene) return;
        auto& c = *static_cast<Ctx*>(user);
        if ((scene->pending_dirty() & DirtyFlags::Layout) != DirtyFlags::None) {
            if (auto* h = interface_cast<IHierarchy>(scene)) {
                c.solver->solve(*h, scene->get_geometry());
            }
        }
        scene->update(c.info->info);
    }, &ctx);
}

} // namespace velk::ui
