#include "frame_snippet_registry.h"

#include "material/material_unpacker.h"

#include <velk/api/object.h>
#include <velk/api/velk.h>
#include <velk-render/interface/intf_frame_data_manager.h>
#include <velk-render/detail/intf_gpu_resource_manager_internal.h>
#include <velk-render/interface/intf_gpu_resource_manager.h>
#include <velk-render/interface/intf_analytic_shape.h>
#include <velk-render/interface/intf_draw_data.h>
#include <velk-render/interface/material/intf_material.h>
#include <velk-render/interface/intf_gpu_arena.h>
#include <velk-render/interface/material/intf_material_internal.h>
#include <velk-render/interface/intf_program.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/interface/intf_shader_source.h>
#include <velk-render/interface/intf_shadow_technique.h>

#include <cstdlib>
#include <cstring>

namespace velk {

void FrameSnippetRegistry::begin_frame()
{
    frame_material_instances_.clear();
    frame_materials_.clear();
    frame_shadow_techs_.clear();
    frame_intersects_.clear();
}

namespace {

/// Looks up an IShaderSource on @p src_carrier, queries (fn, body) for
/// @p role, and registers them as `<fn>.glsl` plus calls
/// register_includes(). Returns the resolved (id, include_name)
/// (creating a new entry on first sight per class uid). Returns
/// id == 0 when the source has no body for @p role.
template <class InfoT>
uint32_t resolve_snippet_id(IInterface* src_carrier, string_view role,
                            IRenderContext& ctx,
                            vector<InfoT>& info_by_id,
                            std::unordered_map<uint64_t, uint32_t>& id_by_class,
                            uint32_t first_id)
{
    if (!src_carrier) return 0;
    auto* src = interface_cast<IShaderSource>(src_carrier);
    if (!src) return 0;
    auto fn = src->get_fn_name(role);
    auto body = src->get_source(role);
    if (fn.empty() || body.empty()) return 0;
    auto* obj = interface_cast<IObject>(src_carrier);
    if (!obj) return 0;
    Uid uid = obj->get_class_uid();
    uint64_t key = uid.hi ^ uid.lo;

    auto it = id_by_class.find(key);
    if (it != id_by_class.end()) {
        return it->second;
    }

    string include_name;
    include_name.append(fn);
    include_name.append(string_view(".glsl", 5));
    ctx.register_shader_include(include_name, body);
    src->register_includes(ctx);

    uint32_t id = static_cast<uint32_t>(info_by_id.size()) + first_id;
    info_by_id.push_back({fn, std::move(include_name)});
    id_by_class[key] = id;
    return id;
}

/// Rewrites a material's compute-side include so its record is read by index
/// out of the shared material arena instead of through a device address.
///
/// The `VELK_MATERIAL(T)` line every material places between its struct
/// declarations and its eval function is the splice point: it is replaced by a
/// generated `T velk_unpack_T(uint)`. The compute prelude defines
/// VELK_LOAD_MATERIAL to paste the type name onto that function, so the
/// snippet's own eval body needs no per-material macro handling.
///
/// Returns false, leaving the include as registered, when the record's layout
/// cannot be derived. The prelude's VELK_MATERIAL(T) is a no-op there, so such
/// a material fails to compile rather than silently reading wrong bytes.
bool install_indexed_material_include(const string& include_name,
                                      string_view body, IRenderContext& ctx)
{
    const string_view type = find_material_type(body);
    if (type.size() == 0) return false;

    const MaterialLayout layout = parse_material_layout(body, type);
    if (!layout.ok) {
        VELK_LOG(E, "indexed material: cannot derive layout for %.*s: %s",
                 static_cast<int>(type.size()), type.data(), layout.error.c_str());
        return false;
    }

    string fn_name("velk_unpack_");
    fn_name.append(type);

    // Everything up to the marker, then the unpacker, then the rest.
    const string_view marker("VELK_MATERIAL(");
    const size_t start = body.find(marker);
    if (start == string_view::npos) return false;
    const size_t line_end = body.find('\n', start);
    if (line_end == string_view::npos) return false;

    string out(body.data(), start);
    out.append(generate_material_unpacker(fn_name, type, layout));
    out.append(body.substr(line_end + 1));

    ctx.register_shader_include(include_name, out);
    return true;
}

void mark_frame_active(vector<uint32_t>& frame_ids, uint32_t id)
{
    for (auto fs : frame_ids) {
        if (fs == id) return;
    }
    frame_ids.push_back(id);
}

} // namespace

uint32_t FrameSnippetRegistry::register_material(IProgram* prog, IRenderContext& ctx)
{
    const size_t before = material_info_by_id_.size();
    uint32_t id = resolve_snippet_id(prog, shader_role::kEval, ctx,
                                     material_info_by_id_, material_id_by_class_,
                                     /*first_id=*/1);
    // First sight of this material class: try to give its compute-side
    // include an indexed record reader.
    if (id != 0 && material_info_by_id_.size() > before) {
        auto& info = material_info_by_id_[id - 1];
        if (auto* src = interface_cast<IShaderSource>(prog)) {
            info.indexed = install_indexed_material_include(
                info.include_name, src->get_source(shader_role::kEval), ctx);
        }
    }
    return id;
}

uint32_t FrameSnippetRegistry::register_shadow_tech(IShadowTechnique* tech, IRenderContext& ctx)
{
    uint32_t id = resolve_snippet_id(tech, shader_role::kShadow, ctx,
                                     shadow_tech_info_by_id_, shadow_tech_id_by_class_,
                                     /*first_id=*/1);
    if (id != 0) mark_frame_active(frame_shadow_techs_, id);
    return id;
}

uint32_t FrameSnippetRegistry::register_intersect(IAnalyticShape* shape, IRenderContext& ctx)
{
    // First visual-contributed kind = 3 (rect/cube/sphere hold 0/1/2).
    uint32_t id = resolve_snippet_id(shape, shader_role::kIntersect, ctx,
                                     intersect_info_by_id_, intersect_id_by_class_,
                                     /*first_id=*/3);
    if (id != 0) mark_frame_active(frame_intersects_, id);
    return id;
}

IFrameSnippetRegistry::MaterialRef
FrameSnippetRegistry::resolve_material(IProgram* prog, const FrameResolveContext& ctx)
{
    if (!prog) return {};
    for (auto& entry : frame_material_instances_) {
        if (entry.prog == prog) {
            return {entry.mat_id, entry.mat_base};
        }
    }

    uint32_t id = ctx.render_ctx ? register_material(prog, *ctx.render_ctx) : 0;
    if (id == 0) {
        frame_material_instances_.push_back({prog, 0, 0});
        return {};
    }

    uint32_t base = 0;

    // The material's record lives in the shared material arena, in an
    // arena-backed IBuffer it serialises straight into. The buffer is ensured
    // here rather than relied on: shapes are stamped during the BVH build,
    // which runs ahead of the renderer's material upload sweep, and a cached
    // BVH would otherwise hold a stale base indefinitely.
    //
    // The shape carries a *word* base, so the byte offset is divided by 4
    // here; the raster path divides the same offset by the record size.
    {
        auto* mi = interface_cast<IMaterialInternal>(prog);
        auto* dd = interface_cast<IDrawData>(prog);
        if (mi && dd && ctx.material_arena) {
            const uint64_t need = dd->get_draw_data_size();
            if (need > 0) {
                auto buf = mi->material_buffer();
                const bool fresh = (!buf || buf->get_data_size() != need);
                if (fresh) {
                    buf = ctx.material_arena->create_buffer(need, need);
                    mi->set_material_buffer(buf);
                }
                if (buf) {
                    // Gated like the upload sweep; whichever of the two runs
                    // first for a given material consumes the dirty flag.
                    const uint64_t tex_gen =
                        ctx.resources ? ctx.resources->texture_generation() : 0;
                    if (fresh || mi->take_material_dirty(tex_gen)) {
                        buf->write(static_cast<size_t>(need), [&](void* dst, size_t n) {
                            dd->write_draw_data(dst, n, ctx.resources);
                        });
                    }
                    base = static_cast<uint32_t>(get_gpu_ref(buf).get_base() / 4u);
                }
            }
        }
    }

    frame_material_instances_.push_back({prog, id, base});
    bool seen = false;
    for (auto fm : frame_materials_) {
        if (fm == id) { seen = true; break; }
    }
    if (!seen) frame_materials_.push_back(id);
    return {id, base};
}

} // namespace velk
