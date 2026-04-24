#ifndef VELK_UI_GLTF_ASSET_IMPL_H
#define VELK_UI_GLTF_ASSET_IMPL_H

#include <velk/ext/object.h>
#include <velk/string.h>
#include <velk/vector.h>

#include <velk-render/interface/intf_mesh.h>
#include <velk-render/interface/intf_surface.h>
#include <velk-render/interface/material/intf_material.h>

#include <velk-ui/plugins/gltf/plugin.h>
#include <velk-ui/plugins/gltf/interface/intf_gltf_asset.h>

namespace velk::ui::impl {

/**
 * @brief Concrete IGltfAsset.
 *
 * Holds the parsed glTF structure and the shared GPU-side objects built
 * at decode time: buffers (one per glTF buffer), images / materials /
 * meshes (one per glTF index). `instantiate()` walks the parsed scene
 * and produces a fresh element tree referencing these shared objects.
 *
 * Memory entries registered under `memory://` for embedded glTF images
 * are retained here so the destructor can unregister them when the
 * asset is freed.
 */
class GltfAsset final : public ::velk::ext::Object<GltfAsset, IGltfAsset>
{
public:
    VELK_CLASS_UID(::velk::ui::ClassId::GltfAsset, "GltfAsset");

    GltfAsset() = default;
    ~GltfAsset() override;

    /// Populates the asset from a cgltf-parsed root. Takes ownership of
    /// `data` (will be released on destruction) and copies the source
    /// bytes (cgltf references into them for bufferviews).
    void load(string_view uri, void* cgltf_data,
              vector<uint8_t> source_bytes,
              vector<IMeshBuffer::Ptr> buffers,
              vector<IMeshBuffer::Ptr> uv1_buffers,
              vector<ISurface::Ptr> images,
              vector<IMaterial::Ptr> materials,
              vector<IMesh::Ptr> meshes,
              vector<string> memory_uris);

    /// Marks the asset as failed to load.
    void init_failed(string_view uri);

    // IResource
    string_view uri() const override { return uri_; }
    bool exists() const override { return loaded_; }
    int64_t size() const override { return static_cast<int64_t>(source_bytes_.size()); }
    bool is_persistent() const override { return persistent_; }
    void set_persistent(bool value) override { persistent_ = value; }

    // IGltfAsset
    IStore::Ptr instantiate() const override;

private:
    string uri_;
    bool loaded_{false};
    bool persistent_{false};

    void* cgltf_data_{};                    ///< cgltf_data*; opaque here to keep cgltf out of the header.
    vector<uint8_t> source_bytes_;          ///< Owned source (cgltf references into this).

    /// Per-glTF-buffer GPU upload (shared VBO+IBO storage owning the
    /// interleaved per-mesh-primitive data). Index = our internal
    /// mesh-buffer index (one per glTF mesh in this round).
    vector<IMeshBuffer::Ptr> buffers_;

    /// Optional parallel UV1 streams, indexed alongside `buffers_`.
    /// Empty Ptr when the corresponding mesh has no TEXCOORD_1.
    vector<IMeshBuffer::Ptr> uv1_buffers_;

    vector<ISurface::Ptr> images_;          ///< Per glTF image.
    vector<IMaterial::Ptr> materials_;      ///< Per glTF material.
    vector<IMesh::Ptr> meshes_;             ///< Per glTF mesh.
    vector<string> memory_uris_;            ///< memory:// paths to unregister on destruction.
};

} // namespace velk::ui::impl

#endif // VELK_UI_GLTF_ASSET_IMPL_H
