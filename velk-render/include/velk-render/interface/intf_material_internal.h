#ifndef VELK_RENDER_INTF_MATERIAL_INTERNAL_H
#define VELK_RENDER_INTF_MATERIAL_INTERNAL_H

#include <velk/interface/intf_interface.h>

#include <cstdint>

namespace velk {

/**
 * @brief Internal interface for setting the pipeline handle on a material.
 *
 * Used by factory methods (e.g. IRenderContext::create_shader_material) to
 * inject the compiled pipeline handle after creation.
 */
class IMaterialInternal : public Interface<IMaterialInternal>
{
public:
    virtual void set_pipeline_handle(uint64_t handle) = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_MATERIAL_INTERNAL_H
