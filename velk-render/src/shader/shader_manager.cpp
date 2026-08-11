#include "shader/shader_manager.h"

#include "shader/shader.h"
#include "shader/velk_glsl.h"

#include <velk/api/velk.h>
#include <velk/hash.h>
#include <velk/interface/intf_plugin.h>

namespace velk::impl {

bool ShaderManager::init()
{
    // The compiler ships as its own plugin (velk_glsl), the only thing that
    // links shaderc, so it can be swapped without touching velk-render. Its
    // library is loaded by name alongside the other plugins by the runtime;
    // this only instantiates the already-registered plugin type.
    get_or_load_plugin<IPlugin>(PluginId::GlslCompilerPlugin);
    compiler_ = instance().create<IShaderCompiler>(ClassId::GlslShaderCompiler);
    if (!compiler_) {
        VELK_LOG(E, "ShaderManager::init: no shader compiler (velk_glsl plugin missing?)");
        return false;
    }

    // The framework-level velk.glsl dependency, alongside any that plugins
    // register later. Its content reaches shader cache keys through the
    // compiler's dependency hash.
    compiler_->register_include("velk.glsl", kVelkGlsl);
    return true;
}

IShader::Ptr ShaderManager::compile(string_view source, ShaderStage stage, uint64_t key)
{
    if (source.empty()) {
        return nullptr;
    }

    if (key == 0) {
        key = make_hash64(source);
    }

    cache_.ensure_initialized();

    // Combined cache key: source key XOR stage discriminator XOR the
    // compiler's dependency hash. Folding the dependency hash in means that
    // any change to a registered dependency (e.g. velk.glsl) naturally
    // invalidates affected entries; old entries with the previous content
    // become orphans rather than corrupt cache hits.
    constexpr uint64_t kStageVertexMix = 0x68f3df8b8e0c8b8dULL;
    constexpr uint64_t kStageFragmentMix = 0xa24baed4963ee407ULL;
    constexpr uint64_t kStageComputeMix = 0x5a3b1d2f6e9c8411ULL;
    uint64_t stage_mix = kStageFragmentMix;
    switch (stage) {
    case ShaderStage::Vertex:   stage_mix = kStageVertexMix;   break;
    case ShaderStage::Fragment: stage_mix = kStageFragmentMix; break;
    case ShaderStage::Compute:  stage_mix = kStageComputeMix;  break;
    }
    uint64_t cache_key = key ^ stage_mix ^ compiler_->dependency_hash();

    auto cached = cache_.read(cache_key);
    if (!cached.empty()) {
        auto shader = instance().create<IShader>(Shader::static_class_id());
        if (shader) {
            shader->init(std::move(cached));
            return shader;
        }
    }

    auto spirv = compiler_->compile(source, stage);
    if (spirv.empty()) {
        return nullptr;
    }

    cache_.write(cache_key, spirv);

    auto shader = instance().create<IShader>(Shader::static_class_id());
    if (!shader) {
        return nullptr;
    }
    shader->init(std::move(spirv));
    return shader;
}

void ShaderManager::register_include(string_view name, string_view content)
{
    compiler_->register_include(name, content);
}

} // namespace velk::impl
