#ifndef VELK_RENDER_INTF_MATERIAL_INTERNAL_H
#define VELK_RENDER_INTF_MATERIAL_INTERNAL_H

#include <velk/interface/intf_interface.h>
#include <velk/interface/types.h>
#include <velk/vector.h>

#include <velk-render/interface/material/intf_material.h>

namespace velk {

struct ShaderParam;

/**
 * @brief Internal interface for configuring a material after creation.
 *
 * Used by factory methods (e.g. IRenderContext::create_shader_material) to
 * inject reflected shader parameters and shader sources. Pipeline handle
 * storage is handled via IProgram::set_pipeline_handle on the program side.
 */
class IMaterialInternal : public Interface<IMaterialInternal, IMaterial>
{
public:
    /// Set up dynamic input properties from reflected shader parameters.
    /// Default implementation does nothing (e.g. ext::Material ignores this).
    virtual ReturnValue setup_inputs(const vector<ShaderParam>& /*params*/)
    {
        return ReturnValue::NothingToDo;
    }

    /// Provide the raw GLSL sources for materials that bypass the
    /// eval-driver (e.g. ShaderMaterial). The renderer will compile the
    /// pipeline lazily on first draw, reading the current IMaterialOptions
    /// attachment — so options set between creation and first draw are
    /// honored. Default implementation does nothing.
    virtual void set_sources(string_view /*vertex_source*/, string_view /*fragment_source*/) {}
};

} // namespace velk

#endif // VELK_RENDER_INTF_MATERIAL_INTERNAL_H
