#include "glsl_plugin.h"

#include "glsl_shader_compiler.h"

namespace velk::glsl {

ReturnValue GlslPlugin::initialize(IVelk& velk, PluginConfig&)
{
    // One compiler per RenderContext, created at context init.
    ::velk::TypeOptions alloc;
    alloc.policy = ::velk::CreationPolicy::Alloc;
    return register_type<impl::GlslShaderCompiler>(velk, alloc);
}

ReturnValue GlslPlugin::shutdown(IVelk&)
{
    return ReturnValue::Success;
}

} // namespace velk::glsl
