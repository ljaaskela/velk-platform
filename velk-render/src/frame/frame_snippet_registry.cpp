#include "frame_snippet_registry.h"

#include <velk/api/object.h>
#include <velk/api/velk.h>
#include <velk-render/interface/intf_frame_data_manager.h>
#include <velk-render/detail/intf_gpu_resource_manager_internal.h>
#include <velk-render/interface/intf_gpu_resource_manager.h>
#include <velk-render/interface/intf_analytic_shape.h>
#include <velk-render/interface/intf_draw_data.h>
#include <velk-render/interface/material/intf_material.h>
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
    frame_data_buffers_.clear();
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
    return resolve_snippet_id(prog, shader_role::kEval, ctx,
                              material_info_by_id_, material_id_by_class_,
                              /*first_id=*/1);
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

namespace {

// Synchronously makes the persistent program-data buffer backend-ready:
// allocates (or reallocates on size change) the GPU buffer, sets the
// IBuffer's GPU address, and uploads the bytes if they are dirty.
void ensure_data_buffer_uploaded(IBuffer* buf, const FrameResolveContext& ctx)
{
    if (!buf || !ctx.render_ctx || !ctx.resources) return;
    if (!ctx.render_ctx->backend()) return;
    size_t bsize = buf->get_data_size();
    if (bsize == 0) return;

    GpuBufferDesc bdesc{};
    bdesc.size = bsize;
    bdesc.cpu_writable = true;
    auto* be = ctx.resources->ensure_buffer_storage(buf, bdesc);
    if (!be) return;
    if (buf->is_dirty()) {
        const uint8_t* bytes = buf->get_data();
        if (bytes) {
            if (auto gb = be->buffer.lock()) {
                if (auto* dst = gb->map()) {
                    std::memcpy(dst, bytes, bsize);
                }
            }
            buf->clear_dirty();
        }
    }
}

} // namespace

IFrameSnippetRegistry::MaterialRef
FrameSnippetRegistry::resolve_material(IProgram* prog, const FrameResolveContext& ctx)
{
    if (!prog) return {};
    for (auto& entry : frame_material_instances_) {
        if (entry.prog == prog) {
            return {entry.mat_id, entry.mat_addr};
        }
    }

    uint32_t id = ctx.render_ctx ? register_material(prog, *ctx.render_ctx) : 0;
    if (id == 0) {
        frame_material_instances_.push_back({prog, 0, 0});
        return {};
    }

    uint64_t addr = 0;

    // Preferred path: the material owns a persistent data buffer whose
    // GPU address is stable across frames.
    IBuffer::Ptr data_buf;
    if (auto* dd = interface_cast<IDrawData>(prog)) {
        data_buf = dd->get_data_buffer(ctx.resources);
    }
    if (data_buf) {
        ensure_data_buffer_uploaded(data_buf.get(), ctx);
        addr = get_gpu_address(data_buf);
        frame_data_buffers_.push_back(data_buf);
    } else if (auto* dd = interface_cast<IDrawData>(prog)) {
        // Fallback: no persistent buffer → serialise into the frame
        // scratch arena. Per-frame address; shape caches across frames
        // would produce stale addresses with this path.
        size_t sz = dd->get_draw_data_size();
        if (sz > 0 && ctx.frame_buffer) {
            void* scratch = std::malloc(sz);
            if (scratch) {
                std::memset(scratch, 0, sz);
                if (dd->write_draw_data(scratch, sz, ctx.resources) == ReturnValue::Success) {
                    addr = ctx.frame_buffer->write(scratch, sz);
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

uint64_t FrameSnippetRegistry::resolve_data_buffer(IDrawData* dd, const FrameResolveContext& ctx)
{
    if (!dd) return 0;
    auto data_buf = dd->get_data_buffer(ctx.resources);
    if (!data_buf) return 0;
    ensure_data_buffer_uploaded(data_buf.get(), ctx);
    uint64_t addr = get_gpu_address(data_buf);
    if (addr == 0) return 0;
    frame_data_buffers_.push_back(data_buf);
    return addr;
}

} // namespace velk
