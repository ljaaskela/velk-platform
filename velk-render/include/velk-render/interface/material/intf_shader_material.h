#ifndef VELK_RENDER_INTF_SHADER_MATERIAL_H
#define VELK_RENDER_INTF_SHADER_MATERIAL_H

#include <velk/api/object_ref.h>
#include <velk/interface/intf_metadata.h>

#include <velk-render/interface/material/intf_material.h>

namespace velk {

/**
 * @brief Interface for shader materials with dynamic inputs.
 *
 * Extends IMaterial with an `inputs` ObjectRef property. The inputs object
 * holds dynamic properties that correspond to the shader's material parameters,
 * discovered via shader reflection. The inputs object can be pre-populated
 * (e.g. from JSON) before the shader is loaded.
 */
class IShaderMaterial : public Interface<IShaderMaterial, IMaterial>
{
public:
    VELK_INTERFACE(
        (PROP, ObjectRef, inputs, {})
    )
};

} // namespace velk

#endif // VELK_RENDER_INTF_SHADER_MATERIAL_H
