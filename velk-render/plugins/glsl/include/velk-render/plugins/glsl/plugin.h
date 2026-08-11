#ifndef VELK_RENDER_PLUGINS_GLSL_PLUGIN_H
#define VELK_RENDER_PLUGINS_GLSL_PLUGIN_H

// velk_glsl sub-plugin: registers GlslShaderCompiler, an IShaderCompiler
// backed by shaderc.
//
// This is the only place shaderc is linked. RenderContext loads the plugin
// lazily, the first time a shader misses the SPIR-V cache, so an application
// shipping a fully populated cache never loads it and does not have to ship
// it. A cache miss with no compiler available fails with a diagnostic naming
// the shader rather than producing a null pipeline.
//
// Public IDs live in velk-render/plugin.h as ClassId::GlslShaderCompiler and
// PluginId::GlslCompilerPlugin, alongside PluginId::RtPlugin, so velk_render
// can reach them without depending on this plugin.

#include <velk-render/plugin.h>

#endif // VELK_RENDER_PLUGINS_GLSL_PLUGIN_H
