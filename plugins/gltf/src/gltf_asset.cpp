#include "gltf_asset.h"

#include <velk/api/attachment.h>
#include <velk/api/hierarchy.h>
#include <velk/api/object.h>
#include <velk/api/state.h>
#include <velk/api/store.h>
#include <velk/api/velk.h>
#include <velk/interface/intf_interface.h>
#include <velk/interface/resource/intf_resource_protocol.h>
#include <velk/interface/resource/intf_resource_store.h>

#include <velk-render/api/shadow_technique.h>
#include <velk-render/plugin.h>
#include <velk-scene/api/element.h>
#include <velk-scene/api/trait/light.h>
#include <velk-scene/api/trait/trs.h>
#include <velk-scene/api/visual/mesh.h>

#include <cmath>

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include <cstdio>

namespace velk::ui::impl {

GltfAsset::~GltfAsset()
{
    if (!memory_uris_.empty()) {
        auto proto = ::velk::instance().resource_store().find_protocol("memory");
        if (auto* mem = interface_cast<IMemoryProtocol>(proto.get())) {
            for (auto& path : memory_uris_) {
                mem->remove_file(path);
            }
        }
    }
    if (cgltf_data_) {
        cgltf_free(static_cast<cgltf_data*>(cgltf_data_));
        cgltf_data_ = nullptr;
    }
}

void GltfAsset::load(string_view uri, void* data,
                     vector<uint8_t> source_bytes,
                     vector<IMeshBuffer::Ptr> buffers,
                     vector<IMeshBuffer::Ptr> uv1_buffers,
                     vector<ISurface::Ptr> images,
                     vector<IMaterial::Ptr> materials,
                     vector<IMesh::Ptr> meshes,
                     vector<string> memory_uris)
{
    uri_ = string(uri);
    cgltf_data_ = data;
    source_bytes_ = std::move(source_bytes);
    buffers_ = std::move(buffers);
    uv1_buffers_ = std::move(uv1_buffers);
    images_ = std::move(images);
    materials_ = std::move(materials);
    meshes_ = std::move(meshes);
    memory_uris_ = std::move(memory_uris);
    loaded_ = true;
}

void GltfAsset::init_failed(string_view uri)
{
    uri_ = string(uri);
    loaded_ = false;
}

namespace {

// glTF Y-up to velk Y-down conversion is a Y-axis reflection. Vertex
// positions and normals are negated on Y at mesh build time (see
// gltf_decoder.cpp), and the index winding is reversed. The same
// reflection has to apply to every node TRS so the scene graph stays
// consistent: child positions land in the right place. For a TRS that
// looks like translate(t) * rotate(q) * scale(s), the Y-mirror is
// translate(t.x, -t.y, t.z) * rotate(-q.x, q.y, -q.z, q.w) * scale(s),
// derived from M * R * M with M = diag(1, -1, 1).
void apply_node_transform(::velk::Trs& trs, const cgltf_node& node)
{
    if (node.has_matrix) {
        // Matrix nodes: decompose into TRS. Standard decomposition assuming
        // the matrix is a regular rigid+scale transform (no shear).
        const float* m = node.matrix;
        ::velk::vec3 t{m[12], m[13], m[14]};
        ::velk::vec3 c0{m[0], m[1], m[2]};
        ::velk::vec3 c1{m[4], m[5], m[6]};
        ::velk::vec3 c2{m[8], m[9], m[10]};
        auto length = [](const ::velk::vec3& v) {
            return std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
        };
        float sx = length(c0), sy = length(c1), sz = length(c2);
        if (sx > 0) { c0.x /= sx; c0.y /= sx; c0.z /= sx; }
        if (sy > 0) { c1.x /= sy; c1.y /= sy; c1.z /= sy; }
        if (sz > 0) { c2.x /= sz; c2.y /= sz; c2.z /= sz; }
        // Rotation matrix -> quaternion (Shepperd's method).
        float trace = c0.x + c1.y + c2.z;
        ::velk::quat q;
        if (trace > 0) {
            float s = std::sqrt(trace + 1.f) * 2.f;
            q.w = 0.25f * s;
            q.x = (c1.z - c2.y) / s;
            q.y = (c2.x - c0.z) / s;
            q.z = (c0.y - c1.x) / s;
        } else if (c0.x > c1.y && c0.x > c2.z) {
            float s = std::sqrt(1.f + c0.x - c1.y - c2.z) * 2.f;
            q.w = (c1.z - c2.y) / s;
            q.x = 0.25f * s;
            q.y = (c1.x + c0.y) / s;
            q.z = (c2.x + c0.z) / s;
        } else if (c1.y > c2.z) {
            float s = std::sqrt(1.f + c1.y - c0.x - c2.z) * 2.f;
            q.w = (c2.x - c0.z) / s;
            q.x = (c1.x + c0.y) / s;
            q.y = 0.25f * s;
            q.z = (c2.y + c1.z) / s;
        } else {
            float s = std::sqrt(1.f + c2.z - c0.x - c1.y) * 2.f;
            q.w = (c0.y - c1.x) / s;
            q.x = (c2.x + c0.z) / s;
            q.y = (c2.y + c1.z) / s;
            q.z = 0.25f * s;
        }
        trs.set_translate({t.x, -t.y, t.z});
        trs.set_rotation_quat({-q.x, q.y, -q.z, q.w});
        trs.set_scale({sx, sy, sz});
    } else {
        if (node.has_translation) {
            trs.set_translate({node.translation[0], -node.translation[1], node.translation[2]});
        }
        if (node.has_rotation) {
            ::velk::quat q{-node.rotation[0], node.rotation[1], -node.rotation[2], node.rotation[3]};
            trs.set_rotation_quat(q);
        }
        if (node.has_scale) {
            trs.set_scale({node.scale[0], node.scale[1], node.scale[2]});
        }
    }
}

void instantiate_node(const cgltf_data* data, const cgltf_node* node,
                      const vector<IMesh::Ptr>& meshes,
                      ::velk::Hierarchy& hier, ::velk::Store& store,
                      const IObject::Ptr& parent_obj,
                      int& counter)
{
    using namespace ::velk;
    using namespace ::velk::ui;

    auto elem = create_element();
    auto trs = ::velk::trait::transform::create_trs();
    apply_node_transform(trs, *node);
    elem.add_trait(trs);

    if (node->mesh) {
        size_t mesh_idx = static_cast<size_t>(node->mesh - data->meshes);
        if (mesh_idx < meshes.size() && meshes[mesh_idx]) {
            auto vis = trait::visual::create_mesh();
            vis.set_mesh(meshes[mesh_idx]);
            elem.add_trait(vis);
        }
    }

    // KHR_lights_punctual. cgltf parses the extension into a flat
    // data->lights[] array and points node->light at the entry. We map
    // type / color / intensity / range and convert spot cone angles
    // from radians (glTF spec) to degrees (velk's ILight state).
    //
    // glTF intensities are physical (lux for directional, candela for
    // point/spot); velk's shader treats intensity as a unitless
    // multiplier with no exposure / tone mapping. Until the renderer
    // grows that, scale physical units down by 1/1000 so typical
    // outdoor values (low thousands) land near 1..10 on our scale.
    if (node->light) {
        const cgltf_light& l = *node->light;
        constexpr float kIntensityScale = 1.f / 1000.f;
        const float intensity = l.intensity * kIntensityScale;
        ::velk::Light light;
        switch (l.type) {
        case cgltf_light_type_directional:
            light = ::velk::trait::render::create_directional_light(
                ::velk::color{l.color[0], l.color[1], l.color[2], 1.f}, intensity);
            break;
        case cgltf_light_type_point:
            light = ::velk::trait::render::create_point_light(
                ::velk::color{l.color[0], l.color[1], l.color[2], 1.f}, intensity,
                l.range > 0.f ? l.range : 1000.f);
            break;
        case cgltf_light_type_spot: {
            constexpr float kRadToDeg = 57.29577951f;
            light = ::velk::trait::render::create_spot_light(
                ::velk::color{l.color[0], l.color[1], l.color[2], 1.f}, intensity,
                l.range > 0.f ? l.range : 1000.f,
                l.spot_inner_cone_angle * kRadToDeg,
                l.spot_outer_cone_angle * kRadToDeg);
            break;
        }
        default:
            break;
        }
        if (light) {
            // Attach an RT shadow technique to directional lights only.
            // Point/spot lights from gltf are typically interior fixtures
            // and adding rt_shadow per-light gets expensive quickly; sun
            // shadows give the biggest visual win for the cost.
            if (l.type == cgltf_light_type_directional) {
                light.add_technique(::velk::technique::create_rt_shadow());
            }
            elem.add_trait(light);
        }
    }

    char id_buf[64];
    std::snprintf(id_buf, sizeof(id_buf), "gltf:node:%d", counter++);
    auto elem_obj = interface_pointer_cast<IObject>(static_cast<IElement::Ptr>(elem));
    store.add(id_buf, elem_obj);

    if (parent_obj) {
        hier.add(parent_obj, elem_obj);
    } else {
        hier.set_root(elem_obj);
    }

    for (cgltf_size i = 0; i < node->children_count; ++i) {
        instantiate_node(data, node->children[i], meshes, hier, store, elem_obj, counter);
    }
}

} // namespace

IStore::Ptr GltfAsset::instantiate() const
{
    if (!loaded_ || !cgltf_data_) {
        return nullptr;
    }
    auto* data = static_cast<const cgltf_data*>(cgltf_data_);
    if (data->scenes_count == 0) {
        return nullptr;
    }

    auto store = ::velk::create_store();
    auto hier = ::velk::create_hierarchy();

    // Use the default scene if specified, else the first scene.
    const cgltf_scene* scene = data->scene ? data->scene : &data->scenes[0];

    // glTF allows a scene to have multiple root nodes. We wrap them
    // under a single synthetic root so the IHierarchy has exactly one
    // root (matches IScene::load expectations).
    //
    // glTF's Y-up to velk's Y-down conversion is done at vertex copy
    // time in gltf_decoder.cpp's mesh build: positions and normals are
    // negated on Y, and triangle winding is swapped to compensate. That
    // keeps the asset's X and Z axes intact so signage faces the same
    // direction as in the source asset; only Y flips, which is what we
    // actually want.
    auto root = ::velk::create_element();
    auto root_obj = interface_pointer_cast<IObject>(static_cast<IElement::Ptr>(root));
    store.add("gltf:node:root", root_obj);
    hier.set_root(root_obj);

    int counter = 0;
    for (cgltf_size i = 0; i < scene->nodes_count; ++i) {
        instantiate_node(data, scene->nodes[i], meshes_, hier, store, root_obj, counter);
    }

    auto hier_obj = interface_pointer_cast<IObject>(hier.as_ptr<IHierarchy>());
    store.add("hierarchy:scene", hier_obj);

    return static_cast<IStore::Ptr>(store);
}

} // namespace velk::ui::impl
