#ifndef VELK_UI_GL_BACKEND_H
#define VELK_UI_GL_BACKEND_H

#include <velk/ext/object.h>
#include <velk-ui/plugins/gl/plugin.h>
#include <velk-ui/plugins/render/intf_render_backend.h>

#include <cstdint>
#include <unordered_map>

namespace velk_ui {

class GlBackend : public velk::ext::Object<GlBackend, IRenderBackend>
{
public:
    VELK_CLASS_UID(ClassId::GlBackend, "GlBackend");

    ~GlBackend() override;

    bool init(void* params) override;
    void shutdown() override;

    bool create_surface(uint64_t surface_id, const SurfaceDesc& desc) override;
    void destroy_surface(uint64_t surface_id) override;
    void update_surface(uint64_t surface_id, const SurfaceDesc& desc) override;

    bool register_pipeline(uint64_t pipeline_key, const PipelineDesc& desc) override;
    velk::vector<UniformInfo> get_pipeline_uniforms(uint64_t pipeline_key) const override;
    void upload_texture(uint64_t texture_key,
                        const uint8_t* pixels, int width, int height) override;

    void begin_frame(uint64_t surface_id) override;
    void submit(velk::array_view<const RenderBatch> batches) override;
    void end_frame() override;

private:
    struct PipelineEntry
    {
        uint32_t program = 0;
        uint32_t vao = 0;
        uint32_t vbo = 0;
        velk::vector<UniformInfo> uniforms;
    };

    struct SurfaceInfo
    {
        int width = 0;
        int height = 0;
    };

    // Globals UBO (binding 0): mat4 projection + vec4 rect = 80 bytes (std140)
    static constexpr uint32_t kGlobalsUboBinding = 0;
    static constexpr uint32_t kGlobalsUboSize = 80;
    uint32_t globals_ubo_ = 0;

    // Material UBO (binding 1): per-batch material uniforms
    static constexpr uint32_t kMaterialUboBinding = 1;
    static constexpr uint32_t kMaterialUboMaxSize = 256;
    uint32_t material_ubo_ = 0;

    std::unordered_map<uint64_t, uint32_t> textures_;
    std::unordered_map<uint64_t, PipelineEntry> pipelines_;
    std::unordered_map<uint64_t, SurfaceInfo> surfaces_;
    uint64_t current_surface_ = 0;

    float projection_[16]{};
    bool initialized_ = false;
};

} // namespace velk_ui

#endif // VELK_UI_GL_BACKEND_H
