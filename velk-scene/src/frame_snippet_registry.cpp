#include "frame_snippet_registry.h"

#include "frame_data_manager.h"
#include "gpu_resource_manager.h"
#include "view_renderer.h"

#include <velk/api/object.h>
#include <velk/api/velk.h>
#include <velk-render/interface/intf_analytic_shape.h>
#include <velk-render/interface/intf_draw_data.h>
#include <velk-render/interface/material/intf_material.h>
#include <velk-render/interface/intf_program.h>
#include <velk-render/interface/intf_render_backend.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/interface/intf_shader_snippet.h>
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

uint32_t FrameSnippetRegistry::register_material(IProgram* prog, IRenderContext& ctx)
{
    if (!prog) return 0;

    auto* mat = interface_cast<IMaterial>(prog);
    if (!mat) return 0;
    auto fn = mat->get_eval_fn_name();
    auto src = mat->get_eval_src();
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
    mat->register_eval_includes(ctx);

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

namespace {

// Synchronously makes the persistent program-data buffer backend-ready:
// allocates (or reallocates on size change) the GPU buffer, sets the
// IBuffer's GPU address, and uploads the bytes if they are dirty. The
// address is valid immediately on return so the caller can stamp it
// into shape records this frame.
void ensure_data_buffer_uploaded(IBuffer* buf, FrameContext& ctx)
{
    if (!buf || !ctx.backend || !ctx.resources) return;
    size_t bsize = buf->get_data_size();
    if (bsize == 0) return;

    auto* be = ctx.resources->find_buffer(buf);
    bool need_alloc = (be == nullptr);
    if (!need_alloc && be->size != bsize) {
        ctx.resources->defer_buffer_destroy(
            be->handle, ctx.present_counter + ctx.latency_frames);
        ctx.resources->unregister_buffer(buf);
        be = nullptr;
        need_alloc = true;
    }
    if (need_alloc) {
        GpuBufferDesc bdesc{};
        bdesc.size = bsize;
        bdesc.cpu_writable = true;
        GpuResourceManager::BufferEntry bentry{};
        bentry.handle = ctx.backend->create_buffer(bdesc);
        bentry.size = bsize;
        ctx.resources->register_buffer(buf, bentry);
        be = ctx.resources->find_buffer(buf);
        buf->set_gpu_address(ctx.backend->gpu_address(bentry.handle));
    }
    if (buf->is_dirty()) {
        const uint8_t* bytes = buf->get_data();
        if (bytes && be) {
            if (auto* dst = ctx.backend->map(be->handle)) {
                std::memcpy(dst, bytes, bsize);
            }
            buf->clear_dirty();
        }
    }
}

} // namespace

FrameSnippetRegistry::MaterialRef
FrameSnippetRegistry::resolve_material(IProgram* prog, FrameContext& ctx)
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
    // GPU address is stable across frames. We make sure it's backend-
    // allocated and uploaded (if dirty) before reading the address so
    // the shape record we stamp below always points at fresh bytes.
    // The buffer outlives the material if needed: we hold a strong ref
    // for the frame, and the IGpuResource observer defers GPU-handle
    // destruction to a safe window (see GpuResourceManager).
    IBuffer::Ptr data_buf;
    if (auto* dd = interface_cast<IDrawData>(prog)) {
        data_buf = dd->get_data_buffer(ctx.resources);
    }
    if (data_buf) {
        ensure_data_buffer_uploaded(data_buf.get(), ctx);
        addr = data_buf->get_gpu_address();
        frame_data_buffers_.push_back(data_buf);
    } else if (auto* dd = interface_cast<IDrawData>(prog)) {
        // Fallback: no persistent buffer → serialise into the frame
        // scratch arena as before. Address is per-frame; shape caches
        // across frames will produce stale addresses with this path.
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

uint64_t FrameSnippetRegistry::resolve_data_buffer(IDrawData* dd, FrameContext& ctx)
{
    if (!dd) return 0;
    auto data_buf = dd->get_data_buffer(ctx.resources);
    if (!data_buf) return 0;
    ensure_data_buffer_uploaded(data_buf.get(), ctx);
    uint64_t addr = data_buf->get_gpu_address();
    if (addr == 0) return 0;
    frame_data_buffers_.push_back(data_buf);
    return addr;
}

} // namespace velk
