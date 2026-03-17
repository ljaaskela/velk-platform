#ifndef VELK_UI_GL_RENDERER_H
#define VELK_UI_GL_RENDERER_H

#include <velk-ui/interface/intf_renderer.h>
#include <velk-ui/interface/intf_element.h>
#include <velk/ext/object.h>

#include <cstdint>
#include <vector>

namespace velk_ui {

/// Per-instance GPU data. 32 bytes, matches IElement::State layout.
struct InstanceData
{
    float x, y, width, height;
    float r, g, b, a;
};

/// CPU-side bookkeeping per visual slot.
struct VisualEntry
{
    velk::IObject::Ptr element;
    std::vector<velk::IFunction::ConstPtr> listeners;
    bool dirty = false;
    bool alive = false;
};

class GlRenderer : public velk::ext::Object<GlRenderer, IRenderer>
{
public:
    VELK_CLASS_UID("2302c979-1531-4d0b-bab6-d1bac99f0a11", "GlRenderer");

    ~GlRenderer() override;

    // IRenderer pure virtuals
    VisualId add_visual(velk::IObject::Ptr element) override;
    void remove_visual(VisualId id) override;

    bool init(int width, int height) override;
    void render() override;
    void shutdown() override;

private:
    void sync_slot(uint32_t slot, velk::IObject* element);

    std::vector<InstanceData> cpu_buffer_;
    std::vector<VisualEntry> entries_;
    std::vector<uint32_t> free_slots_;
    std::vector<uint32_t> dirty_slots_;

    uint32_t vao_ = 0;
    uint32_t vbo_quad_ = 0;
    uint32_t vbo_instance_ = 0;
    uint32_t shader_program_ = 0;
    int uniform_projection_ = -1;
    bool initialized_ = false;
};

} // namespace velk_ui

#endif // VELK_UI_GL_RENDERER_H
