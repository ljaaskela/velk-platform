#include "glsl_shader_compiler.h"

#include <velk/api/perf.h>
#include <velk/api/velk.h>
#include <velk/hash.h>

#include <algorithm>
#include <cstring>
#include <shaderc/shaderc.hpp>

namespace velk::impl {
namespace {

/// Resolves `#include "name"` against the registered include set instead of
/// the filesystem.
class VelkIncluder : public shaderc::CompileOptions::IncluderInterface
{
public:
    explicit VelkIncluder(const std::unordered_map<string, string>* includes) : includes_(includes) {}

    shaderc_include_result* GetInclude(const char* requested_source, shaderc_include_type, const char*,
                                       size_t) override
    {
        auto* result = new shaderc_include_result{};

        if (includes_) {
            auto it = includes_->find(requested_source);
            if (it != includes_->end()) {
                // Store the name so it outlives this call
                cached_name_ = it->first;
                result->source_name = cached_name_.c_str();
                result->source_name_length = cached_name_.size();
                result->content = it->second.c_str();
                result->content_length = it->second.size();
            } else {
                set_error(result, requested_source);
            }
        } else {
            set_error(result, requested_source);
        }

        return result;
    }

    void ReleaseInclude(shaderc_include_result* data) override { delete data; }

private:
    void set_error(shaderc_include_result* result, const char* name)
    {
        error_msg_ = string("unknown include: ") + name;
        result->source_name = "";
        result->source_name_length = 0;
        result->content = error_msg_.c_str();
        result->content_length = error_msg_.size();
    }

    const std::unordered_map<string, string>* includes_;
    string cached_name_;
    string error_msg_;
};

} // namespace

vector<uint32_t> GlslShaderCompiler::compile(string_view source, ShaderStage stage, string* out_error)
{
    VELK_PERF_SCOPE("vk.compile_glsl_to_spirv");
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;

    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
    options.SetTargetSpirv(shaderc_spirv_version_1_5);
    options.SetGenerateDebugInfo();
    options.SetIncluder(std::make_unique<VelkIncluder>(includes_.empty() ? nullptr : &includes_));

    shaderc_shader_kind kind = shaderc_fragment_shader;
    const char* filename = "fragment.glsl";
    switch (stage) {
    case ShaderStage::Vertex:
        kind = shaderc_vertex_shader;
        filename = "vertex.glsl";
        break;
    case ShaderStage::Fragment:
        kind = shaderc_fragment_shader;
        filename = "fragment.glsl";
        break;
    case ShaderStage::Compute:
        kind = shaderc_compute_shader;
        filename = "compute.glsl";
        break;
    }

    auto result = compiler.CompileGlslToSpv(source.data(), kind, filename, options);

    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        VELK_LOG(E, "Shader compilation failed: %s", result.GetErrorMessage().c_str());
        if (out_error) {
            *out_error = string(result.GetErrorMessage().c_str());
        }
        return {};
    }

    return {result.cbegin(), result.cend()};
}

void GlslShaderCompiler::register_include(string_view name, string_view content)
{
    includes_[name] = content;
}

uint64_t GlslShaderCompiler::dependency_hash() const
{
    // Sort by name so the hash is deterministic across runs (unordered_map
    // iteration order may differ).
    vector<const std::pair<const string, string>*> sorted;
    sorted.reserve(includes_.size());
    for (auto& kv : includes_) {
        sorted.push_back(&kv);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](auto* a, auto* b) {
                  return std::strcmp(a->first.c_str(), b->first.c_str()) < 0;
              });

    uint64_t h = 0xcbf29ce484222325ULL;
    for (auto* kv : sorted) {
        // Mix in name and content separately so that ("ab","c") and
        // ("a","bc") don't collide.
        h ^= make_hash64(kv->first);
        h *= 0x100000001b3ULL;
        h ^= make_hash64(kv->second);
        h *= 0x100000001b3ULL;
    }
    return h;
}

} // namespace velk::impl
