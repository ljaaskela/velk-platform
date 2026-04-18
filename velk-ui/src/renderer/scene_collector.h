#ifndef VELK_UI_SCENE_COLLECTOR_H
#define VELK_UI_SCENE_COLLECTOR_H

#include "view_renderer.h"

#include <velk/api/state.h>
#include <velk/interface/intf_object_storage.h>

#include <velk-render/gpu_data.h>
#include <velk-render/interface/intf_analytic_shape.h>
#include <velk-render/interface/intf_light.h>
#include <velk-render/interface/intf_program.h>
#include <velk-render/interface/intf_shadow_technique.h>
#include <velk-render/render_types.h>
#include <velk-ui/interface/intf_visual.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace velk::ui {

// GPU-side shape record. Mirrors the RtShape struct declared in the RT
// compute prelude (and the deferred compute shader). Geometry + material
// + texture + shape discriminator in 128 bytes.
VELK_GPU_STRUCT RtShape
{
    float    origin[4];       ///< xyz = world origin (corner for rect/cube, AABB corner for sphere)
    float    u_axis[4];       ///< xyz = local x axis scaled by width
    float    v_axis[4];       ///< xyz = local y axis scaled by height
    float    w_axis[4];       ///< xyz = local z axis scaled by depth (cube only; zero otherwise)
    float    color[4];        ///< rgba base color (used when material_id == 0)
    float    params[4];       ///< x = corner radius (rect) or sphere radius; yzw reserved
    uint32_t material_id;     ///< 0 = no material (use color); otherwise dispatched via switch
    uint32_t texture_id;      ///< bindless index, 0 when unused
    uint32_t shape_param;     ///< per-shape material data (e.g. glyph index for text)
    uint32_t shape_kind;      ///< 0 = rect, 1 = cube, 2 = sphere
    uint64_t material_data_addr;
    uint64_t _tail_pad;
};
static_assert(sizeof(RtShape) == 128, "RtShape layout mismatch");

// GPU-side scene light. Mirrors the `Light` struct in the compute shaders
// (80 bytes). flags.y is shadow_tech_id; callers populate it after
// resolving their shadow technique registry.
struct GpuLight
{
    uint32_t flags[4];            ///< x = LightType, y = shadow_tech_id, zw = _
    float    position[4];         ///< xyz = world position (point / spot)
    float    direction[4];        ///< xyz = world forward (dir / spot)
    float    color_intensity[4];  ///< rgb = colour, a = intensity multiplier
    float    params[4];           ///< x = range, y = cos(inner), z = cos(outer)
};
static_assert(sizeof(GpuLight) == 80, "GpuLight layout mismatch");

// What enumerate_scene_shapes passes to its callback. `geometry` is
// pre-filled with origin/axes/color/params/shape_kind/shape_param; the
// callback mutates material_id / material_data_addr / texture_id and
// pushes the record into its own output vector.
//
// `draw_entry` is non-null for rect-path shapes (one call per draw
// entry from IVisual::get_draw_entries) and null for analytic shapes
// (cube / sphere, one call per visual).
struct ShapeSite
{
    RtShape geometry;
    IVisual* visual = nullptr;
    IProgram* paint = nullptr;
    const DrawEntry* draw_entry = nullptr;
};

// What enumerate_scene_lights passes to its callback. `base` has every
// field set except flags[1] (shadow_tech_id); the callback resolves any
// attached IShadowTechnique, assigns the id, and pushes the record.
struct LightSite
{
    GpuLight base;
    IElement* element = nullptr;
    ILight* light = nullptr;
};

// Returns the first IShadowTechnique attached to the given light, or
// nullptr if none. Both RayTracer and DeferredLighter resolve lights to
// a shadow tech this way; the lookup is centralised here.
inline IShadowTechnique* find_shadow_technique(ILight* light)
{
    if (!light) return nullptr;
    auto* obj = interface_cast<IObject>(light);
    auto* storage = interface_cast<IObjectStorage>(obj);
    if (!storage) return nullptr;
    for (size_t k = 0; k < storage->attachment_count(); ++k) {
        if (auto* st = interface_cast<IShadowTechnique>(storage->get_attachment(k))) {
            return st;
        }
    }
    return nullptr;
}

// Walks all visuals in scene_state and invokes `cb(ShapeSite&)` for each
// geometric shape found:
//   - One call per analytic shape (cube / sphere) with draw_entry == null.
//   - One call per rect draw entry (IVisual::get_draw_entries) with
//     draw_entry pointing at the source entry.
//
// `cb` may mutate geometry.material_id / material_data_addr / texture_id
// (and whatever else) before pushing its own copy into an output vector.
template <typename F>
void enumerate_scene_shapes(const SceneState& scene_state, F&& cb)
{
    for (auto& ve : scene_state.visual_list) {
        if (ve.type != VisualEntry::Element || !ve.element) continue;
        auto es = read_state<IElement>(ve.element);
        if (!es) continue;
        auto* storage = interface_cast<IObjectStorage>(ve.element);
        if (!storage) continue;
        const float ox = es->world_matrix(0, 3);
        const float oy = es->world_matrix(1, 3);
        const float oz = es->world_matrix(2, 3);
        const float ux = es->world_matrix(0, 0);
        const float uy = es->world_matrix(1, 0);
        const float uz = es->world_matrix(2, 0);
        const float vx = es->world_matrix(0, 1);
        const float vy = es->world_matrix(1, 1);
        const float vz = es->world_matrix(2, 1);
        const float wx = es->world_matrix(0, 2);
        const float wy = es->world_matrix(1, 2);
        const float wz = es->world_matrix(2, 2);
        const float ew = es->size.width;
        const float eh = es->size.height;
        const float ed = es->size.depth;

        for (size_t j = 0; j < storage->attachment_count(); ++j) {
            auto* visual = interface_cast<IVisual>(storage->get_attachment(j));
            if (!visual) continue;
            auto vs = read_state<IVisual>(visual);
            if (!vs) continue;

            IProgram* paint = nullptr;
            if (vs->paint) {
                auto prog_ptr = vs->paint.template get<IProgram>();
                paint = prog_ptr.get();
            }

            auto* analytic = interface_cast<IAnalyticShape>(visual);
            uint32_t shape_kind = analytic ? analytic->get_shape_kind() : 0;

            if (shape_kind != 0) {
                ShapeSite site{};
                site.visual = visual;
                site.paint = paint;
                auto& s = site.geometry;
                s.origin[0] = ox; s.origin[1] = oy; s.origin[2] = oz;
                s.u_axis[0] = ux * ew; s.u_axis[1] = uy * ew; s.u_axis[2] = uz * ew;
                s.v_axis[0] = vx * eh; s.v_axis[1] = vy * eh; s.v_axis[2] = vz * eh;
                s.w_axis[0] = wx * ed; s.w_axis[1] = wy * ed; s.w_axis[2] = wz * ed;
                s.color[0] = vs->color.r;
                s.color[1] = vs->color.g;
                s.color[2] = vs->color.b;
                s.color[3] = vs->color.a;
                if (shape_kind == 2) {
                    s.params[0] = std::min({ew, eh, ed}) * 0.5f;
                }
                s.shape_kind = shape_kind;
                cb(site);
                continue;
            }

            // Rect path: one shape per draw entry.
            float radius = 0.f;
            if (analytic && !analytic->get_shape_intersect_source().empty()) {
                radius = std::min(std::min(ew, eh) * 0.5f, 12.f);
            }

            rect local_rect{0, 0, ew, eh};
            auto entries = visual->get_draw_entries(local_rect);
            for (auto& dentry : entries) {
                // Instance layout (instance_types.h):
                //   [ 0..63 ] mat4 world_matrix (raster only)
                //   [64..71] vec2 pos
                //   [72..79] vec2 size
                //   [80..95] vec4 color
                //   [96..99] uint glyph_index (TextInstance only)
                constexpr size_t kPosOff = 64;
                constexpr size_t kSizeOff = 72;
                constexpr size_t kColorOff = 80;
                constexpr size_t kGlyphIdxOff = 96;
                if (dentry.instance_size < kSizeOff + 8) continue;

                float local_px = 0, local_py = 0, sz_w = 0, sz_h = 0;
                std::memcpy(&local_px, dentry.instance_data + kPosOff, 4);
                std::memcpy(&local_py, dentry.instance_data + kPosOff + 4, 4);
                std::memcpy(&sz_w, dentry.instance_data + kSizeOff, 4);
                std::memcpy(&sz_h, dentry.instance_data + kSizeOff + 4, 4);

                float cr = vs->color.r, cg = vs->color.g, cb_ = vs->color.b, ca = vs->color.a;
                if (dentry.instance_size >= kColorOff + 16) {
                    std::memcpy(&cr, dentry.instance_data + kColorOff, 4);
                    std::memcpy(&cg, dentry.instance_data + kColorOff + 4, 4);
                    std::memcpy(&cb_, dentry.instance_data + kColorOff + 8, 4);
                    std::memcpy(&ca, dentry.instance_data + kColorOff + 12, 4);
                }

                uint32_t shape_param = 0;
                if (dentry.instance_size >= kGlyphIdxOff + 4) {
                    std::memcpy(&shape_param, dentry.instance_data + kGlyphIdxOff, 4);
                }

                ShapeSite site{};
                site.visual = visual;
                site.paint = paint;
                site.draw_entry = &dentry;
                auto& s = site.geometry;
                s.origin[0] = ox + ux * local_px + vx * local_py;
                s.origin[1] = oy + uy * local_px + vy * local_py;
                s.origin[2] = oz + uz * local_px + vz * local_py;
                s.u_axis[0] = ux * sz_w; s.u_axis[1] = uy * sz_w; s.u_axis[2] = uz * sz_w;
                s.v_axis[0] = vx * sz_h; s.v_axis[1] = vy * sz_h; s.v_axis[2] = vz * sz_h;
                s.color[0] = cr; s.color[1] = cg; s.color[2] = cb_; s.color[3] = ca;
                s.params[0] = radius;
                s.shape_param = shape_param;
                cb(site);
            }
        }
    }
}

// Walks all lights in scene_state and invokes `cb(LightSite&)` for each
// ILight attachment found. `base` has every field populated except
// flags[1] (shadow_tech_id); the callback resolves a shadow technique
// (see find_shadow_technique) and stores the id before pushing.
template <typename F>
void enumerate_scene_lights(const SceneState& scene_state, F&& cb)
{
    constexpr float kDegToRad = 0.017453292519943295f;

    for (auto& ve : scene_state.visual_list) {
        if (ve.type != VisualEntry::Element || !ve.element) continue;
        auto es = read_state<IElement>(ve.element);
        if (!es) continue;
        auto* storage = interface_cast<IObjectStorage>(ve.element);
        if (!storage) continue;

        for (size_t j = 0; j < storage->attachment_count(); ++j) {
            auto* light = interface_cast<ILight>(storage->get_attachment(j));
            if (!light) continue;
            auto ls = read_state<ILight>(light);
            if (!ls) continue;

            LightSite site{};
            site.element = ve.element.get();
            site.light = light;
            auto& g = site.base;
            g.flags[0] = static_cast<uint32_t>(ls->type);
            g.position[0] = es->world_matrix(0, 3);
            g.position[1] = es->world_matrix(1, 3);
            g.position[2] = es->world_matrix(2, 3);
            float fx = -es->world_matrix(0, 2);
            float fy = -es->world_matrix(1, 2);
            float fz = -es->world_matrix(2, 2);
            float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
            if (flen > 1e-6f) { fx /= flen; fy /= flen; fz /= flen; }
            g.direction[0] = fx;
            g.direction[1] = fy;
            g.direction[2] = fz;
            g.color_intensity[0] = ls->color.r;
            g.color_intensity[1] = ls->color.g;
            g.color_intensity[2] = ls->color.b;
            g.color_intensity[3] = ls->intensity;
            g.params[0] = ls->range;
            g.params[1] = std::cos(ls->cone_inner_deg * kDegToRad);
            g.params[2] = std::cos(ls->cone_outer_deg * kDegToRad);
            cb(site);
        }
    }
}

} // namespace velk::ui

#endif // VELK_UI_SCENE_COLLECTOR_H
