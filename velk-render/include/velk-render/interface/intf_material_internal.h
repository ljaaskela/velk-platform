#ifndef VELK_RENDER_INTF_MATERIAL_INTERNAL_H
#define VELK_RENDER_INTF_MATERIAL_INTERNAL_H

#include <velk/interface/intf_interface.h>
#include <velk/interface/types.h>
#include <velk/vector.h>

#include <cstdint>
#include <velk-render/interface/intf_material.h>

namespace velk {

struct ShaderParam;

/**
 * @brief Internal interface for configuring a material after creation.
 *
 * Used by factory methods (e.g. IRenderContext::create_shader_material) to
 * inject the compiled pipeline handle and shader parameters.
 */
class IMaterialInternal : public Interface<IMaterialInternal, IMaterial>
{
public:
    virtual void set_pipeline_handle(uint64_t handle) = 0;

    /// Set up dynamic input properties from reflected shader parameters.
    /// Default implementation does nothing (e.g. ext::Material ignores this).
    virtual ReturnValue setup_inputs(const vector<ShaderParam>& /*params*/)
    {
        return ReturnValue::NothingToDo;
    }
};

} // namespace velk

#endif // VELK_RENDER_INTF_MATERIAL_INTERNAL_H
