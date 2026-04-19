#include "frame_snippet_registry.h"

#include "frame_data_manager.h"

#include <velk/api/object.h>
#include <velk/api/velk.h>
#include <velk-render/interface/intf_analytic_shape.h>
#include <velk-render/interface/intf_draw_data.h>
#include <velk-render/interface/intf_program.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/interface/intf_shader_snippet.h>
#include <velk-render/interface/intf_shadow_technique.h>

#include <cstdlib>
#include <cstring>

namespace velk::ui {

void FrameSnippetRegistry::begin_frame()
{
    frame_material_instances_.clear();
    frame_materials_.clear();
    frame_shadow_techs_.clear();
    frame_intersects_.clear();
}

uint32_t FrameSnippetRegistry::register_material(IProgram* prog, IRenderContext& ctx)
{
    if (!prog) return 0;
    auto* snippet = interface_cast<IShaderSnippet>(prog);
    if (!snippet) return 0;
    auto fn = snippet->get_snippet_fn_name();
    auto src = snippet->get_snippet_source();
    if (fn.empty() || src.empty()) return 0;
    auto* obj = interface_cast<IObject>(prog);
    if (!obj) return 0;
    Uid uid = obj->get_class_uid();
    uint64_t key = uid.hi ^ uid.lo;

    auto it = material_id_by_class_.find(key);
    if (it != material_id_by_class_.end()) {
        return it->second;
    }

    string include_name;
    include_name.append(fn);
    include_name.append(string_view(".glsl", 5));
    ctx.register_shader_include(include_name, src);
    snippet->register_snippet_includes(ctx);

    uint32_t id = static_cast<uint32_t>(material_info_by_id_.size()) + 1;
    material_info_by_id_.push_back({fn, std::move(include_name)});
    material_id_by_class_[key] = id;
    return id;
}

uint32_t FrameSnippetRegistry::register_shadow_tech(IShadowTechnique* tech, IRenderContext& ctx)
{
    if (!tech) return 0;
    auto fn = tech->get_snippet_fn_name();
    auto src = tech->get_snippet_source();
    if (fn.empty() || src.empty()) return 0;
    auto* obj = interface_cast<IObject>(tech);
    if (!obj) return 0;
    Uid uid = obj->get_class_uid();
    uint64_t key = uid.hi ^ uid.lo;

    auto it = shadow_tech_id_by_class_.find(key);
    uint32_t id;
    if (it != shadow_tech_id_by_class_.end()) {
        id = it->second;
    } else {
        string include_name;
        include_name.append(fn);
        include_name.append(string_view(".glsl", 5));
        ctx.register_shader_include(include_name, src);
        tech->register_snippet_includes(ctx);
        id = static_cast<uint32_t>(shadow_tech_info_by_id_.size()) + 1;
        shadow_tech_info_by_id_.push_back({fn, std::move(include_name)});
        shadow_tech_id_by_class_[key] = id;
    }

    // Mark active for this frame's pipeline composition.
    bool seen = false;
    for (auto fs : frame_shadow_techs_) {
        if (fs == id) { seen = true; break; }
    }
    if (!seen) frame_shadow_techs_.push_back(id);
    return id;
}

uint32_t FrameSnippetRegistry::register_intersect(IAnalyticShape* shape, IRenderContext& ctx)
{
    if (!shape) return 0;
    auto fn = shape->get_shape_intersect_fn_name();
    auto src = shape->get_shape_intersect_source();
    if (fn.empty() || src.empty()) return 0;
    auto* obj = interface_cast<IObject>(shape);
    if (!obj) return 0;
    Uid uid = obj->get_class_uid();
    uint64_t key = uid.hi ^ uid.lo;

    auto it = intersect_id_by_class_.find(key);
    uint32_t id;
    if (it != intersect_id_by_class_.end()) {
        id = it->second;
    } else {
        string include_name;
        include_name.append(fn);
        include_name.append(string_view(".glsl", 5));
        ctx.register_shader_include(include_name, src);
        shape->register_shape_intersect_includes(ctx);
        // First visual-contributed kind = 3 (rect/cube/sphere hold 0/1/2).
        id = static_cast<uint32_t>(intersect_info_by_id_.size()) + 3;
        intersect_info_by_id_.push_back({fn, std::move(include_name)});
        intersect_id_by_class_[key] = id;
    }

    bool seen = false;
    for (auto fi : frame_intersects_) {
        if (fi == id) { seen = true; break; }
    }
    if (!seen) frame_intersects_.push_back(id);
    return id;
}

FrameSnippetRegistry::MaterialRef
FrameSnippetRegistry::resolve_material(IProgram* prog, IRenderContext& ctx, FrameDataManager& fb)
{
    if (!prog) return {};
    for (auto& entry : frame_material_instances_) {
        if (entry.prog == prog) {
            return {entry.mat_id, entry.mat_addr};
        }
    }

    uint32_t id = register_material(prog, ctx);
    if (id == 0) {
        frame_material_instances_.push_back({prog, 0, 0});
        return {};
    }

    uint64_t addr = 0;
    if (auto* dd = interface_cast<IDrawData>(prog)) {
        size_t sz = dd->get_draw_data_size();
        if (sz > 0) {
            void* scratch = std::malloc(sz);
            if (scratch) {
                std::memset(scratch, 0, sz);
                if (dd->write_draw_data(scratch, sz) == ReturnValue::Success) {
                    addr = fb.write(scratch, sz);
                }
                std::free(scratch);
            }
        }
    }

    frame_material_instances_.push_back({prog, id, addr});
    bool seen = false;
    for (auto fm : frame_materials_) {
        if (fm == id) { seen = true; break; }
    }
    if (!seen) frame_materials_.push_back(id);
    return {id, addr};
}

} // namespace velk::ui
