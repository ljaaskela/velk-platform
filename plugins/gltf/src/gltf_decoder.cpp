#include "gltf_decoder.h"

#include "gltf_asset.h"

#include <velk/api/object.h>
#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/ext/core_object.h>
#include <velk/interface/intf_interface.h>
#include <velk/interface/resource/intf_resource.h>
#include <velk/interface/resource/intf_resource_protocol.h>
#include <velk/interface/resource/intf_resource_store.h>
#include <velk/string.h>
#include <velk/vector.h>

#include <velk-render/api/material/standard_material.h>
#include <velk-render/api/material/material_property.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_image.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/interface/material/intf_material_property.h>
#include <velk-render/render_types.h>

#include <velk-ui/plugins/image/api/image.h>

#include "cgltf.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace velk::ui::impl {

namespace {

constexpr uint32_t kVertexStride = 32;  // vec3 pos + vec3 normal + vec2 uv = 32 bytes (matches VelkVertex3D).

struct InterleavedVertex
{
    float pos[3];
    float nrm[3];
    float uv[2];
};
static_assert(sizeof(InterleavedVertex) == kVertexStride, "InterleavedVertex must be 32 bytes");

bool extension_is_safelisted(const char* name)
{
    if (!name) return false;
    return std::strcmp(name, "KHR_materials_specular") == 0
        || std::strcmp(name, "KHR_materials_emissive_strength") == 0
        || std::strcmp(name, "KHR_texture_transform") == 0;
}

bool check_required_extensions(const cgltf_data* data)
{
    for (cgltf_size i = 0; i < data->extensions_required_count; ++i) {
        if (!extension_is_safelisted(data->extensions_required[i])) {
            VELK_LOG(E, "gltf: required extension '%s' not supported",
                     data->extensions_required[i]);
            return false;
        }
    }
    return true;
}

string make_memory_uri(string_view asset_uri, size_t image_idx, string_view ext)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "gltf-%p-image-%zu", static_cast<const void*>(asset_uri.data()),
                  image_idx);
    string out;
    out.reserve(strlen(buf) + ext.size() + 1);
    out.append(buf);
    out.push_back('.');
    out.append(string(ext));
    return out;
}

string_view ext_from_mime(string_view mime)
{
    if (mime == "image/jpeg") return "jpg";
    if (mime == "image/png") return "png";
    return "bin";
}

// Join a relative URI against the base glTF URI's directory (everything up to
// and including the final '/'). Used for sibling external image files.
string join_sibling_uri(string_view base, string_view rel)
{
    size_t slash = base.rfind('/');
    string out;
    if (slash != string_view::npos) {
        out.append(base.substr(0, slash + 1));
    }
    out.append(rel);
    return out;
}

// cgltf file.read callback: fetch a sibling resource through the velk
// resource store instead of the C stdio default. cgltf hands us a path it
// produced by combining the gltf base URI with the buffer/file URI; we treat
// that path as a velk URI directly.
cgltf_result velk_cgltf_file_read(const cgltf_memory_options* mem_opts,
                                  const cgltf_file_options* /*file_opts*/,
                                  const char* path, cgltf_size* size, void** data)
{
    if (!path || !size || !data) return cgltf_result_invalid_options;

    auto& rs = ::velk::instance().resource_store();
    string_view uri(path, std::strlen(path));
    auto file = rs.get_resource<IFile>(uri);
    if (!file) {
        VELK_LOG(E, "gltf: sibling resource not found '%.*s'",
                 static_cast<int>(uri.size()), uri.data());
        return cgltf_result_file_not_found;
    }

    vector<uint8_t> bytes;
    if (failed(file->read(bytes)) || bytes.empty()) {
        return cgltf_result_io_error;
    }

    auto alloc = mem_opts->alloc_func;
    void* buf = alloc
        ? alloc(mem_opts->user_data, bytes.size())
        : std::malloc(bytes.size());
    if (!buf) return cgltf_result_out_of_memory;
    std::memcpy(buf, bytes.data(), bytes.size());
    *size = bytes.size();
    *data = buf;
    return cgltf_result_success;
}

void velk_cgltf_file_release(const cgltf_memory_options* mem_opts,
                             const cgltf_file_options* /*file_opts*/,
                             void* data, cgltf_size /*size*/)
{
    if (!data) return;
    if (mem_opts->free_func) mem_opts->free_func(mem_opts->user_data, data);
    else std::free(data);
}

ISurface::Ptr decode_image(IMemoryProtocol& mem, IResourceStore& rs,
                           const cgltf_data* data, const cgltf_image& image,
                           string_view asset_uri, size_t image_idx,
                           vector<string>& memory_uris_out)
{
    // Source bytes for this image: either from a bufferView (embedded) or a
    // URI (external file or data: scheme).
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    string_view mime = image.mime_type
        ? string_view(image.mime_type, std::strlen(image.mime_type))
        : string_view("image/png");

    vector<uint8_t> external_bytes;  // owns bytes when image is loaded from a sibling file
    if (image.buffer_view) {
        const cgltf_buffer_view* bv = image.buffer_view;
        if (!bv->buffer || !bv->buffer->data) return nullptr;
        bytes = static_cast<const uint8_t*>(bv->buffer->data) + bv->offset;
        size = bv->size;
    } else if (image.uri) {
        string_view uri_view(image.uri, std::strlen(image.uri));
        if (uri_view.substr(0, 5) == string_view("data:")) {
            VELK_LOG(W, "gltf: data: URI for image not yet supported");
            return nullptr;
        }
        string sibling = join_sibling_uri(asset_uri, uri_view);
        auto file = rs.get_resource<IFile>(sibling);
        if (!file) {
            VELK_LOG(E, "gltf: image sibling '%.*s' not found",
                     static_cast<int>(sibling.size()), sibling.data());
            return nullptr;
        }
        if (failed(file->read(external_bytes)) || external_bytes.empty()) {
            return nullptr;
        }
        bytes = external_bytes.data();
        size = external_bytes.size();
        // Derive mime from extension if cgltf didn't supply one.
        if (!image.mime_type) {
            size_t dot = uri_view.rfind('.');
            if (dot != string_view::npos) {
                string_view ext = uri_view.substr(dot + 1);
                if (ext == "jpg" || ext == "jpeg") mime = "image/jpeg";
                else if (ext == "png") mime = "image/png";
            }
        }
    }

    if (!bytes || size == 0) return nullptr;

    string mem_path = make_memory_uri(asset_uri, image_idx, ext_from_mime(mime));
    if (failed(mem.add_file(mem_path, bytes, size))) {
        return nullptr;
    }
    memory_uris_out.push_back(mem_path);

    // Route through the image: decoder for IImage / ISurface.
    string image_uri;
    image_uri.append("image:memory://");
    image_uri.append(mem_path);
    auto img = rs.get_resource<IImage>(image_uri);
    return interface_pointer_cast<ISurface>(img);
}

SamplerAddressMode translate_wrap(int gl)
{
    switch (gl) {
    case 33071: return SamplerAddressMode::ClampToEdge;     // GL_CLAMP_TO_EDGE
    case 33648: return SamplerAddressMode::MirroredRepeat;  // GL_MIRRORED_REPEAT
    case 10497:                                             // GL_REPEAT
    default:    return SamplerAddressMode::Repeat;
    }
}

SamplerFilter translate_mag(int gl)
{
    return gl == 9728 /*GL_NEAREST*/ ? SamplerFilter::Nearest : SamplerFilter::Linear;
}

// glTF min filters fold mag-filter and mipmap mode into one enum. Decompose.
SamplerFilter translate_min_filter(int gl)
{
    // 9728 NEAREST, 9729 LINEAR
    // 9984 NEAREST_MIPMAP_NEAREST, 9985 LINEAR_MIPMAP_NEAREST
    // 9986 NEAREST_MIPMAP_LINEAR, 9987 LINEAR_MIPMAP_LINEAR
    if (gl == 9728 || gl == 9984 || gl == 9986) return SamplerFilter::Nearest;
    return SamplerFilter::Linear;
}

SamplerMipmapMode translate_min_mipmap(int gl)
{
    // 9728/9729 have no mipmap; default to Linear (matches our default).
    if (gl == 9984 || gl == 9985) return SamplerMipmapMode::Nearest;
    return SamplerMipmapMode::Linear;
}

SamplerDesc sampler_desc_from(const cgltf_sampler* s)
{
    SamplerDesc d{};
    if (!s) return d;
    if (s->wrap_s) d.wrap_s = translate_wrap(s->wrap_s);
    if (s->wrap_t) d.wrap_t = translate_wrap(s->wrap_t);
    if (s->mag_filter) d.mag_filter = translate_mag(s->mag_filter);
    if (s->min_filter) {
        d.min_filter = translate_min_filter(s->min_filter);
        d.mipmap_mode = translate_min_mipmap(s->min_filter);
    }
    return d;
}

IMaterial::Ptr build_material(const cgltf_data* data, const cgltf_material& m,
                              const vector<ISurface::Ptr>& images)
{
    using namespace ::velk;

    auto mat = material::create_standard();

    auto image_for = [&](const cgltf_texture* tex) -> ISurface::Ptr {
        if (!tex || !tex->image) return nullptr;
        size_t idx = static_cast<size_t>(tex->image - data->images);
        return idx < images.size() ? images[idx] : nullptr;
    };

    auto apply_tex_transform = [&](auto& prop, const cgltf_texture_view& tv) {
        prop.set_tex_coord(tv.texcoord);
        if (tv.has_transform) {
            const auto& xt = tv.transform;
            prop.set_uv_offset({xt.offset[0], xt.offset[1]});
            prop.set_uv_rotation(xt.rotation);
            prop.set_uv_scale({xt.scale[0], xt.scale[1]});
            if (xt.has_texcoord) {
                prop.set_tex_coord(xt.texcoord);
            }
        }
    };

    if (m.has_pbr_metallic_roughness) {
        const auto& pbr = m.pbr_metallic_roughness;
        mat.set_base_color({pbr.base_color_factor[0], pbr.base_color_factor[1],
                            pbr.base_color_factor[2], pbr.base_color_factor[3]});
        if (auto img = image_for(pbr.base_color_texture.texture)) {
            mat.base_color().set_texture(img);
            apply_tex_transform(mat.base_color(), pbr.base_color_texture);
        }
        mat.set_metallic(pbr.metallic_factor);
        mat.set_roughness(pbr.roughness_factor);
        if (auto img = image_for(pbr.metallic_roughness_texture.texture)) {
            mat.metallic_roughness().set_texture(img);
            apply_tex_transform(mat.metallic_roughness(), pbr.metallic_roughness_texture);
        }
    }
    if (m.normal_texture.texture) {
        if (auto img = image_for(m.normal_texture.texture)) {
            mat.normal().set_texture(img);
            mat.normal().set_scale(m.normal_texture.scale);
            apply_tex_transform(mat.normal(), m.normal_texture);
        }
    }
    if (m.occlusion_texture.texture) {
        if (auto img = image_for(m.occlusion_texture.texture)) {
            mat.occlusion().set_texture(img);
            mat.occlusion().set_strength(m.occlusion_texture.scale);
            apply_tex_transform(mat.occlusion(), m.occlusion_texture);
        }
    }
    {
        bool has_emissive = m.emissive_factor[0] != 0 || m.emissive_factor[1] != 0
                         || m.emissive_factor[2] != 0
                         || m.emissive_texture.texture != nullptr
                         || m.has_emissive_strength;
        if (has_emissive) {
            mat.emissive().set_factor({m.emissive_factor[0], m.emissive_factor[1],
                                       m.emissive_factor[2], 1.f});
            if (m.has_emissive_strength) {
                mat.emissive().set_strength(m.emissive_strength.emissive_strength);
            }
            if (auto img = image_for(m.emissive_texture.texture)) {
                mat.emissive().set_texture(img);
                apply_tex_transform(mat.emissive(), m.emissive_texture);
            }
        }
    }
    if (m.has_specular) {
        const auto& sp = m.specular;
        mat.specular().set_factor(sp.specular_factor);
        mat.specular().set_color_factor({sp.specular_color_factor[0],
                                          sp.specular_color_factor[1],
                                          sp.specular_color_factor[2], 1.f});
        if (auto img = image_for(sp.specular_texture.texture)) {
            mat.specular().set_texture(img);
            apply_tex_transform(mat.specular(), sp.specular_texture);
        }
    }

    // Legacy KHR_materials_pbrSpecularGlossiness: convert to metal/rough +
    // KHR_materials_specular on the fly, treating surfaces as dielectric.
    // See design-notes/gltf-import.md for the full list of shortcuts.
    if (m.has_pbr_specular_glossiness && !m.has_pbr_metallic_roughness) {
        const auto& sg = m.pbr_specular_glossiness;
        mat.set_base_color({sg.diffuse_factor[0], sg.diffuse_factor[1],
                            sg.diffuse_factor[2], sg.diffuse_factor[3]});
        if (auto img = image_for(sg.diffuse_texture.texture)) {
            mat.base_color().set_texture(img);
            apply_tex_transform(mat.base_color(), sg.diffuse_texture);
        }
        mat.set_metallic(0.f);
        mat.set_roughness(1.f - sg.glossiness_factor);

        const float smax = std::max({sg.specular_factor[0],
                                     sg.specular_factor[1],
                                     sg.specular_factor[2]});
        if (smax > 0.f) {
            // F0 caps at 0.04 in the specular extension; values above
            // (e.g. polished marble's F0 ~= 1.0) get clamped and look
            // matte. Heuristic upgrade to metal is a future improvement.
            mat.specular().set_factor(std::min(smax / 0.04f, 1.f));
            mat.specular().set_color_factor({sg.specular_factor[0] / smax,
                                              sg.specular_factor[1] / smax,
                                              sg.specular_factor[2] / smax, 1.f});
        }

        // Per-pixel data: split specularGlossinessTexture (RGB = sRGB
        // specular tint, A = linear glossiness) into two new textures:
        //   - metallicRoughness: G = 1 - alpha (roughness, linear), B = 0
        //   - specularColor: RGB copied (sRGB), A = 255 (full strength)
        // Source pixels are still CPU-resident since the renderer hasn't
        // uploaded yet at decode time.
        if (sg.specular_glossiness_texture.texture
            && sg.specular_glossiness_texture.texture->image) {
            const auto* src_img_obj = sg.specular_glossiness_texture.texture->image;
            size_t img_idx = static_cast<size_t>(src_img_obj - data->images);
            if (img_idx < images.size() && images[img_idx]) {
                auto src_surf = images[img_idx];
                auto* buf = interface_cast<IBuffer>(src_surf.get());
                const uint8_t* src_pixels = buf ? buf->get_data() : nullptr;
                size_t src_size = buf ? buf->get_data_size() : 0;
                auto dims = src_surf->get_dimensions();
                size_t pixel_count = static_cast<size_t>(dims.x) * dims.y;
                if (src_pixels && src_size == pixel_count * 4) {
                    vector<uint8_t> mr_pixels;
                    mr_pixels.resize(src_size);
                    vector<uint8_t> sp_pixels;
                    sp_pixels.resize(src_size);
                    for (size_t i = 0; i < pixel_count; ++i) {
                        const uint8_t* sp = src_pixels + i * 4;
                        mr_pixels[i * 4 + 0] = 255;          // R: occlusion (unused, default)
                        mr_pixels[i * 4 + 1] = static_cast<uint8_t>(255 - sp[3]);  // G: roughness
                        mr_pixels[i * 4 + 2] = 0;            // B: metallic = 0
                        mr_pixels[i * 4 + 3] = 255;
                        sp_pixels[i * 4 + 0] = sp[0];
                        sp_pixels[i * 4 + 1] = sp[1];
                        sp_pixels[i * 4 + 2] = sp[2];
                        sp_pixels[i * 4 + 3] = 255;          // A: specular strength = 1
                    }
                    char mr_uri[80];
                    std::snprintf(mr_uri, sizeof(mr_uri),
                                  "gltf-mr-%p-%zu",
                                  static_cast<const void*>(data), img_idx);
                    auto mr_img = ::velk::ui::image::create_image_from_pixels(
                        mr_uri, static_cast<int>(dims.x), static_cast<int>(dims.y),
                        PixelFormat::RGBA8, mr_pixels.data(), mr_pixels.size());
                    if (mr_img) {
                        static_cast<IImage::Ptr>(mr_img)->set_sampler_desc(src_surf->get_sampler_desc());
                        mat.metallic_roughness().set_texture(mr_img.as_surface());
                        apply_tex_transform(mat.metallic_roughness(),
                                            sg.specular_glossiness_texture);
                    }
                    char sp_uri[80];
                    std::snprintf(sp_uri, sizeof(sp_uri),
                                  "gltf-sp-%p-%zu",
                                  static_cast<const void*>(data), img_idx);
                    auto sp_img = ::velk::ui::image::create_image_from_pixels(
                        sp_uri, static_cast<int>(dims.x), static_cast<int>(dims.y),
                        PixelFormat::RGBA8_SRGB, sp_pixels.data(), sp_pixels.size());
                    if (sp_img) {
                        static_cast<IImage::Ptr>(sp_img)->set_sampler_desc(src_surf->get_sampler_desc());
                        mat.specular().set_texture(sp_img.as_surface());
                        apply_tex_transform(mat.specular(),
                                            sg.specular_glossiness_texture);
                    }
                }
            }
        }
    }

    return static_cast<IMaterial::Ptr>(mat);
}

bool read_attribute_to_floats(const cgltf_accessor* acc, vector<float>& out, size_t components)
{
    if (!acc) return false;
    out.resize(acc->count * components);
    for (cgltf_size i = 0; i < acc->count; ++i) {
        if (!cgltf_accessor_read_float(acc, i, &out[i * components], components)) {
            return false;
        }
    }
    return true;
}

bool read_indices_to_uint32(const cgltf_accessor* acc, vector<uint32_t>& out)
{
    if (!acc) return false;
    out.resize(acc->count);
    for (cgltf_size i = 0; i < acc->count; ++i) {
        out[i] = static_cast<uint32_t>(cgltf_accessor_read_index(acc, i));
    }
    return true;
}

IMesh::Ptr build_mesh(IMeshBuilder& builder, const cgltf_data* /*data*/,
                      const cgltf_mesh& mesh,
                      const vector<IMaterial::Ptr>& materials,
                      IMeshBuffer::Ptr& uv1_buffer_out)
{
    using namespace ::velk;

    // Pass 1: count totals + detect uv1 presence.
    uint32_t total_vertices = 0;
    uint32_t total_indices = 0;
    bool any_uv1 = false;
    for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi) {
        const cgltf_primitive& p = mesh.primitives[pi];
        if (p.type != cgltf_primitive_type_triangles) continue;
        const cgltf_accessor* pos = nullptr;
        const cgltf_accessor* uv1 = nullptr;
        for (cgltf_size ai = 0; ai < p.attributes_count; ++ai) {
            const auto& a = p.attributes[ai];
            if (a.type == cgltf_attribute_type_position) pos = a.data;
            else if (a.type == cgltf_attribute_type_texcoord && a.index == 1) uv1 = a.data;
        }
        if (!pos || !p.indices) continue;
        total_vertices += static_cast<uint32_t>(pos->count);
        total_indices += static_cast<uint32_t>(p.indices->count);
        if (uv1) any_uv1 = true;
    }

    if (total_vertices == 0 || total_indices == 0) {
        return nullptr;
    }

    vector<InterleavedVertex> vbo;
    vbo.resize(total_vertices);
    vector<uint32_t> ibo;
    ibo.resize(total_indices);
    vector<float> uv1_data;  // packed vec2s (2 * total_vertices floats), or empty if no UV1
    if (any_uv1) uv1_data.assign(total_vertices * 2u, 0.f);

    struct Range
    {
        uint32_t v_offset, v_count, i_offset, i_count;
        const cgltf_material* material;
        aabb bounds;
    };
    vector<Range> ranges;
    ranges.reserve(mesh.primitives_count);

    uint32_t v_cursor = 0;
    uint32_t i_cursor = 0;

    for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi) {
        const cgltf_primitive& p = mesh.primitives[pi];
        if (p.type != cgltf_primitive_type_triangles) continue;

        const cgltf_accessor* pos = nullptr;
        const cgltf_accessor* nrm = nullptr;
        const cgltf_accessor* uv0 = nullptr;
        const cgltf_accessor* uv1 = nullptr;
        for (cgltf_size ai = 0; ai < p.attributes_count; ++ai) {
            const auto& a = p.attributes[ai];
            if (a.type == cgltf_attribute_type_position) pos = a.data;
            else if (a.type == cgltf_attribute_type_normal) nrm = a.data;
            else if (a.type == cgltf_attribute_type_texcoord && a.index == 0) uv0 = a.data;
            else if (a.type == cgltf_attribute_type_texcoord && a.index == 1) uv1 = a.data;
        }
        if (!pos || !p.indices) continue;

        const uint32_t v_count = static_cast<uint32_t>(pos->count);
        const uint32_t i_count = static_cast<uint32_t>(p.indices->count);

        // Positions
        vector<float> positions;
        if (!read_attribute_to_floats(pos, positions, 3)) continue;
        // Normals (default to +Z if missing)
        vector<float> normals;
        if (nrm && !read_attribute_to_floats(nrm, normals, 3)) continue;
        // UV0 (default to 0)
        vector<float> uvs;
        if (uv0 && !read_attribute_to_floats(uv0, uvs, 2)) continue;
        // UV1 (default to 0)
        vector<float> uvs1;
        if (uv1 && !read_attribute_to_floats(uv1, uvs1, 2)) continue;

        // glTF is right-handed Y-up; velk world is Y-down. Negate Y on
        // positions and normals so the asset stands the right way up
        // physically. Single-axis reflection inverts triangle winding,
        // so we compensate below by swapping two indices per triangle.
        for (uint32_t k = 0; k < v_count; ++k) {
            InterleavedVertex& v = vbo[v_cursor + k];
            v.pos[0] =  positions[k * 3 + 0];
            v.pos[1] = -positions[k * 3 + 1];
            v.pos[2] =  positions[k * 3 + 2];
            if (nrm) {
                v.nrm[0] =  normals[k * 3 + 0];
                v.nrm[1] = -normals[k * 3 + 1];
                v.nrm[2] =  normals[k * 3 + 2];
            } else {
                v.nrm[0] = 0; v.nrm[1] = 0; v.nrm[2] = 1;
            }
            if (uv0) {
                v.uv[0] = uvs[k * 2 + 0];
                v.uv[1] = uvs[k * 2 + 1];
            } else {
                v.uv[0] = 0; v.uv[1] = 0;
            }
            if (any_uv1) {
                if (uv1) {
                    uv1_data[(v_cursor + k) * 2 + 0] = uvs1[k * 2 + 0];
                    uv1_data[(v_cursor + k) * 2 + 1] = uvs1[k * 2 + 1];
                } else {
                    uv1_data[(v_cursor + k) * 2 + 0] = 0.f;
                    uv1_data[(v_cursor + k) * 2 + 1] = 0.f;
                }
            }
        }

        // Indices: copy + rebase by v_cursor so each primitive references
        // its own vertex range within the shared VBO. We also swap the
        // 2nd and 3rd vertex of every triangle to compensate for the
        // winding flip introduced by the Y-axis position negation above.
        vector<uint32_t> prim_indices;
        if (!read_indices_to_uint32(p.indices, prim_indices)) continue;
        const uint32_t tri_count = i_count / 3;
        for (uint32_t t = 0; t < tri_count; ++t) {
            ibo[i_cursor + t * 3 + 0] = prim_indices[t * 3 + 0] + v_cursor;
            ibo[i_cursor + t * 3 + 1] = prim_indices[t * 3 + 2] + v_cursor;
            ibo[i_cursor + t * 3 + 2] = prim_indices[t * 3 + 1] + v_cursor;
        }
        // Tail (non-triangle indices) — keep order. cgltf only gives us
        // triangles when the primitive type is triangles, so this is
        // strictly defensive.
        for (uint32_t k = tri_count * 3; k < i_count; ++k) {
            ibo[i_cursor + k] = prim_indices[k] + v_cursor;
        }

        aabb b{};
        if (pos->has_min && pos->has_max) {
            b.position = {pos->min[0], pos->min[1], pos->min[2]};
            b.extent = {pos->max[0] - pos->min[0],
                        pos->max[1] - pos->min[1],
                        pos->max[2] - pos->min[2]};
        }

        Range r{};
        r.v_offset = v_cursor;
        r.v_count = v_count;
        r.i_offset = i_cursor;
        r.i_count = i_count;
        r.material = p.material;
        r.bounds = b;
        ranges.push_back(r);

        v_cursor += v_count;
        i_cursor += i_count;
    }

    if (ranges.empty()) {
        return nullptr;
    }

    auto buffer = builder.build_buffer(vbo.data(), vbo.size() * sizeof(InterleavedVertex),
                                       ibo.data(), ibo.size() * sizeof(uint32_t));
    if (!buffer) return nullptr;

    if (any_uv1) {
        uv1_buffer_out = builder.build_buffer(uv1_data.data(),
                                              uv1_data.size() * sizeof(float),
                                              nullptr, 0);
    } else {
        uv1_buffer_out = nullptr;
    }

    static constexpr VertexAttribute kAttrs[] = {
        {VertexAttributeSemantic::Position,  VertexAttributeFormat::Float3, 0},
        {VertexAttributeSemantic::Normal,    VertexAttributeFormat::Float3, 12},
        {VertexAttributeSemantic::TexCoord0, VertexAttributeFormat::Float2, 24},
    };

    vector<IMeshPrimitive::Ptr> prims;
    prims.reserve(ranges.size());
    for (auto& r : ranges) {
        auto prim = builder.build_primitive_in_buffer(
            buffer,
            r.v_offset * kVertexStride, r.v_count,
            r.i_offset * static_cast<uint32_t>(sizeof(uint32_t)), r.i_count,
            {kAttrs, 3},
            kVertexStride,
            MeshTopology::TriangleList,
            r.bounds,
            uv1_buffer_out,
            r.v_offset * static_cast<uint32_t>(sizeof(float) * 2));
        if (!prim) continue;
        if (r.material) {
            // glTF materials are deduplicated to our `materials` array
            // by index. Look up by pointer offset.
            // (cgltf stores materials in data->materials; we map by index
            //  in build phase.)
        }
        prims.push_back(prim);
    }

    auto mesh_obj = builder.build({prims.data(), prims.size()});
    if (!mesh_obj) return nullptr;

    // Stamp materials on each primitive (post-build).
    auto mesh_prims = mesh_obj->get_primitives();
    for (size_t k = 0; k < mesh_prims.size() && k < ranges.size(); ++k) {
        if (ranges[k].material) {
            // Resolve material index from pointer.
            // Caller passes the materials vector indexed by glTF order.
            // We look up via pointer arithmetic using the cgltf_data root.
            // This is done in the calling scope where `data` is available.
        }
    }

    // Caller patches in materials after we return (they have the cgltf_data
    // root pointer needed for material index lookup).
    (void)materials;
    return mesh_obj;
}

} // namespace

IResource::Ptr GltfDecoder::decode(const IResource::Ptr& inner) const
{
    if (!inner) return nullptr;

    auto* file = interface_cast<IFile>(inner);
    if (!file) return nullptr;

    vector<uint8_t> bytes;
    if (failed(file->read(bytes)) || bytes.empty()) {
        auto obj = ::velk::ext::make_object<GltfAsset>();
        if (auto* asset = static_cast<GltfAsset*>(obj.get())) {
            asset->init_failed(inner->uri());
        }
        return interface_pointer_cast<IResource>(obj);
    }

    cgltf_options opts{};
    opts.file.read = &velk_cgltf_file_read;
    opts.file.release = &velk_cgltf_file_release;
    cgltf_data* data = nullptr;
    if (cgltf_parse(&opts, bytes.data(), bytes.size(), &data) != cgltf_result_success) {
        VELK_LOG(E, "gltf: parse failed for '%.*s'",
                 static_cast<int>(inner->uri().size()), inner->uri().data());
        auto obj = ::velk::ext::make_object<GltfAsset>();
        static_cast<GltfAsset*>(obj.get())->init_failed(inner->uri());
        return interface_pointer_cast<IResource>(obj);
    }

    if (!check_required_extensions(data)) {
        cgltf_free(data);
        auto obj = ::velk::ext::make_object<GltfAsset>();
        static_cast<GltfAsset*>(obj.get())->init_failed(inner->uri());
        return interface_pointer_cast<IResource>(obj);
    }

    // cgltf needs a null-terminated base path for sibling-URI resolution; our
    // IResource exposes only string_view, so make a temporary copy.
    string base_uri(inner->uri());
    if (cgltf_load_buffers(&opts, data, base_uri.c_str()) != cgltf_result_success) {
        VELK_LOG(E, "gltf: load_buffers failed for '%.*s'",
                 static_cast<int>(inner->uri().size()), inner->uri().data());
        cgltf_free(data);
        auto obj = ::velk::ext::make_object<GltfAsset>();
        static_cast<GltfAsset*>(obj.get())->init_failed(inner->uri());
        return interface_pointer_cast<IResource>(obj);
    }

    auto& velk = ::velk::instance();
    auto& rs = velk.resource_store();
    IMemoryProtocol* memory_proto = nullptr;
    auto memory_proto_ptr = rs.find_protocol("memory");
    if (memory_proto_ptr) {
        memory_proto = interface_cast<IMemoryProtocol>(memory_proto_ptr.get());
    }

    // The mesh builder is a plain factory; create a standalone one
    // (no render-context state needed for build_buffer /
    // build_primitive_in_buffer). The renderer's upload pass picks up
    // the resulting IMeshBuffers via the normal dirty-tracking path.
    auto builder_ptr = velk.create<IMeshBuilder>(::velk::ClassId::MeshBuilder);
    if (!builder_ptr) {
        VELK_LOG(E, "gltf: failed to create IMeshBuilder");
        cgltf_free(data);
        auto obj = ::velk::ext::make_object<GltfAsset>();
        static_cast<GltfAsset*>(obj.get())->init_failed(inner->uri());
        return interface_pointer_cast<IResource>(obj);
    }
    auto& builder = *builder_ptr;

    // Build images.
    vector<ISurface::Ptr> images;
    images.resize(data->images_count);
    vector<string> memory_uris;
    if (memory_proto) {
        for (cgltf_size i = 0; i < data->images_count; ++i) {
            images[i] = decode_image(*memory_proto, rs, data, data->images[i],
                                     inner->uri(), i, memory_uris);
        }
    }

    // Apply per-texture sampler settings to the shared images. If two
    // textures reference the same image with different samplers the last
    // one wins — cost of the "one Image per glTF image" sharing model;
    // separate Image instances per (image, sampler) is a round 4 item.
    for (cgltf_size i = 0; i < data->textures_count; ++i) {
        const cgltf_texture& tex = data->textures[i];
        if (!tex.image) continue;
        size_t img_idx = static_cast<size_t>(tex.image - data->images);
        if (img_idx >= images.size() || !images[img_idx]) continue;
        if (auto* img = interface_cast<IImage>(images[img_idx].get())) {
            img->set_sampler_desc(sampler_desc_from(tex.sampler));
        }
    }

    // Build materials.
    vector<IMaterial::Ptr> materials;
    materials.resize(data->materials_count);
    for (cgltf_size i = 0; i < data->materials_count; ++i) {
        materials[i] = build_material(data, data->materials[i], images);
    }

    // Build meshes (one IMeshBuffer per glTF mesh, primitives sharing it).
    vector<IMesh::Ptr> meshes;
    meshes.resize(data->meshes_count);
    vector<IMeshBuffer::Ptr> uv1_buffers;
    uv1_buffers.resize(data->meshes_count);
    vector<IMeshBuffer::Ptr> mesh_buffers;
    mesh_buffers.resize(data->meshes_count);

    for (cgltf_size mi = 0; mi < data->meshes_count; ++mi) {
        IMeshBuffer::Ptr uv1_buf;
        meshes[mi] = build_mesh(builder, data, data->meshes[mi], materials, uv1_buf);
        uv1_buffers[mi] = uv1_buf;

        // Patch each primitive's material from the cgltf primitive's
        // material pointer (resolved against `materials` by index).
        if (meshes[mi]) {
            auto prims = meshes[mi]->get_primitives();
            const cgltf_mesh& m = data->meshes[mi];
            size_t pk = 0;
            for (cgltf_size pi = 0; pi < m.primitives_count; ++pi) {
                const cgltf_primitive& p = m.primitives[pi];
                if (p.type != cgltf_primitive_type_triangles) continue;
                if (pk >= prims.size()) break;
                if (p.material) {
                    size_t idx = static_cast<size_t>(p.material - data->materials);
                    if (idx < materials.size() && materials[idx] && prims[pk]) {
                        ::velk::write_state<IMeshPrimitive>(prims[pk].get(), [&](auto& s) {
                            ::velk::set_object_ref(s.material, materials[idx]);
                        });
                    }
                }
                ++pk;
            }
        }
    }

    // The InitializerList syntax of vector<>'s constructors doesn't carry
    // ownership; use std::move to hand vectors to the asset.
    auto obj = ::velk::ext::make_object<GltfAsset>();
    auto* asset = static_cast<GltfAsset*>(obj.get());
    asset->load(inner->uri(), data, std::move(bytes),
                std::move(mesh_buffers), std::move(uv1_buffers),
                std::move(images), std::move(materials),
                std::move(meshes), std::move(memory_uris));
    return interface_pointer_cast<IResource>(obj);
}

} // namespace velk::ui::impl
