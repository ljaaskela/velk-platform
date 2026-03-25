#ifndef VELK_UI_GL_RENDERER_H
#define VELK_UI_GL_RENDERER_H

#include <velk/ext/object.h>

#include <cstdint>
#include <unordered_map>
#include <velk-ui/interface/intf_element.h>
#include <velk-ui/interface/intf_renderer.h>
#include <velk-ui/interface/intf_texture_provider.h>
#include <velk-ui/types.h>

namespace velk_ui {

/// Per-instance GPU data for untextured rectangles.
struct RectInstanceData
{
    float x, y, width, height;
    velk::color color;
};

/// Per-instance GPU data for textured quads (glyphs).
struct TextInstanceData
{
    float x, y, width, height;
    velk::color color;
    float u0, v0, u1, v1;
};

class GlRenderer : public velk::ext::Object<GlRenderer, IRenderer>
{
public:
    VELK_CLASS_UID("2302c979-1531-4d0b-bab6-d1bac99f0a11", "GlRenderer");

    ~GlRenderer() override;

    // IRenderer pure virtuals
    VisualId add_visual(const IElement::Ptr& element) override;
    void remove_visual(VisualId id) override;
    void update_visuals(velk::array_view<IElement*> changed) override;

    bool init(int width, int height) override;
    void render() override;
    void shutdown() override;

private:
    /// A rect draw command with a custom shader program.
    struct CustomShaderRect
    {
        RectInstanceData instance;
        uint32_t program = 0;
    };

    /// CPU-side bookkeeping per registered element.
    struct ElementEntry
    {
        IElement::Ptr element;
        velk::vector<DrawCommand> cached_commands;
        ITextureProvider* texture_provider = nullptr;
        uint32_t custom_program = 0;  ///< Compiled custom shader, 0 = use default.
        bool alive = false;

        bool is_valid() const { return alive && element; }
    };

    void rebuild_commands(uint32_t slot);
    void rebuild_instances();
    void upload_atlas();

    velk::vector<ElementEntry> entries_;
    velk::vector<uint32_t> free_slots_;
    std::unordered_map<IElement*, uint32_t> object_to_slot_;

    // Rect batch (untextured)
    uint32_t rect_vao_ = 0;
    uint32_t rect_vbo_ = 0;
    uint32_t rect_program_ = 0;
    int rect_proj_uniform_ = -1;

    // Text batch (textured)
    uint32_t text_vao_ = 0;
    uint32_t text_vbo_ = 0;
    uint32_t text_program_ = 0;
    int text_proj_uniform_ = -1;
    int text_atlas_uniform_ = -1;
    uint32_t atlas_texture_ = 0;

    // Instance data rebuilt when dirty
    velk::vector<RectInstanceData> rect_instances_;
    velk::vector<TextInstanceData> text_instances_;
    velk::vector<CustomShaderRect> custom_shader_rects_;

    bool instances_dirty_ = true;
    bool atlas_dirty_ = false;
    bool initialized_ = false;
};

} // namespace velk_ui

#endif // VELK_UI_GL_RENDERER_H
