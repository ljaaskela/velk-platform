#ifndef VELK_UI_GL_BACKEND_H
#define VELK_UI_GL_BACKEND_H

#include <velk/ext/object.h>
#include <velk-ui/plugins/render/intf_render_backend.h>

#include <cstdint>
#include <unordered_map>

namespace velk_ui {

class GlBackend : public velk::ext::Object<GlBackend, IRenderBackend>
{
public:
    VELK_CLASS_UID("2302c979-1531-4d0b-bab6-d1bac99f0a11", "GlBackend");

    ~GlBackend() override;

    bool init(void* params) override;
    void shutdown() override;

    bool create_surface(uint64_t surface_id, const SurfaceDesc& desc) override;
    void destroy_surface(uint64_t surface_id) override;
    void update_surface(uint64_t surface_id, const SurfaceDesc& desc) override;

    bool register_pipeline(uint64_t pipeline_key, const PipelineDesc& desc) override;
    void upload_texture(uint64_t texture_key,
                        const uint8_t* pixels, int width, int height) override;

    void begin_frame(uint64_t surface_id) override;
    void submit(velk::array_view<const RenderBatch> batches) override;
    void end_frame() override;

private:
    struct PipelineEntry
    {
        uint32_t program = 0;
        int proj_uniform = -1;
        int rect_uniform = -1;
        int atlas_uniform = -1;
    };

    struct SurfaceInfo
    {
        int width = 0;
        int height = 0;
    };

    // VAOs and VBOs per vertex format
    uint32_t untextured_vao_ = 0;
    uint32_t untextured_vbo_ = 0;
    uint32_t textured_vao_ = 0;
    uint32_t textured_vbo_ = 0;

    // Texture cache
    std::unordered_map<uint64_t, uint32_t> textures_; // texture_key -> GL texture id

    // Pipeline cache
    std::unordered_map<uint64_t, PipelineEntry> pipelines_;

    // Surface tracking
    std::unordered_map<uint64_t, SurfaceInfo> surfaces_;
    uint64_t current_surface_ = 0;

    // Current frame projection (set in begin_frame)
    float projection_[16]{};

    bool initialized_ = false;
};

} // namespace velk_ui

#endif // VELK_UI_GL_BACKEND_H
