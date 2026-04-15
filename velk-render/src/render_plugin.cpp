#include "render_plugin.h"

#include "render_context.h"
#include "render_texture.h"
#include "shader.h"
#include "shader_material.h"
#include "surface.h"

#include <velk/ext/any.h>
#include <velk-render/render_types.h>

namespace velk {

ReturnValue RenderPlugin::initialize(IVelk& velk, PluginConfig& config)
{
    ::velk::TypeOptions alloc;
    alloc.policy = ::velk::CreationPolicy::Alloc;
    auto rv = register_type<RenderContextImpl>(velk, alloc);
    rv &= register_type<Shader>(velk);
    rv &= register_type<WindowSurface>(velk);
    rv &= register_type<RenderTexture>(velk);
    rv &= register_type<impl::ShaderMaterial>(velk);
    rv &= register_type<::velk::ext::AnyValue<UpdateRate>>(velk);
    return rv;
}

ReturnValue RenderPlugin::shutdown(IVelk&)
{
    return ReturnValue::Success;
}

} // namespace velk
