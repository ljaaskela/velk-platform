#ifndef VELK_UI_INTF_GLTF_ASSET_H
#define VELK_UI_INTF_GLTF_ASSET_H

#include <velk/interface/intf_interface.h>
#include <velk/interface/intf_store.h>
#include <velk/interface/resource/intf_resource.h>

namespace velk::ui {

/**
 * @brief A loaded glTF 2.0 asset.
 *
 * Produced by the "gltf" resource decoder. Holds the parsed glTF structure
 * and shared GPU-side objects (meshes, materials, textures). The asset
 * itself does not contain a live element tree: element trees are single
 * parent / single scene, so a cached asset produces fresh trees on each
 * `instantiate()` call. The GPU data is shared across instantiations.
 *
 * Typical use:
 * @code
 * auto asset = velk::instance().resource_store()
 *     .get_resource<IGltfAsset>("gltf:app://assets/box.glb");
 * if (asset) {
 *     scene->load(*asset->instantiate(), parent_element);
 * }
 * @endcode
 *
 * When `instantiate()` builds the root element, the source `IGltfAsset`
 * is attached to it so scene-side code can reach the asset via
 * `get_attachment<IGltfAsset>(root_element)`.
 *
 * Chain: IInterface -> IResource -> IGltfAsset
 */
class IGltfAsset : public Interface<IGltfAsset, IResource>
{
public:
    /**
     * @brief Builds a fresh element tree (elements + attachments) into a
     *        new IStore. Safe to call any number of times; returned stores
     *        are disjoint element-wise but share the asset's GPU data
     *        (IMesh, StandardMaterial, ITexture).
     *
     * Returns null if the asset failed to load. The returned store uses
     * the `json_importer` convention ("hierarchy:scene" for the root
     * hierarchy), so it can be passed directly to `IScene::load`. A node's
     * element is stored as "gltf:node:<name>"; only the first node with a
     * given name is, and the rest are numbered.
     */
    virtual IStore::Ptr instantiate() const = 0;
};

} // namespace velk::ui

#endif // VELK_UI_INTF_GLTF_ASSET_H
