#ifndef VELK_RENDER_INTF_MATERIAL_H
#define VELK_RENDER_INTF_MATERIAL_H

#include <velk/interface/intf_metadata.h>

#include <cstddef>
#include <cstdint>

namespace velk {

class IRenderContext; // forward declaration

/**
 * @brief Interface for custom materials that override the default color fill.
 *
 * A material defines how a visual's geometry is shaded. When a visual's paint
 * property references an IMaterial, the renderer uses the material's pipeline
 * instead of the visual's color property.
 *
 * Every material provides a pipeline handle and GPU data. The renderer writes
 * the GPU data after the DrawDataHeader in the staging buffer. The material's
 * shader reads it via buffer_reference from the draw data pointer.
 */
class IMaterial : public Interface<IMaterial>
{
public:
    virtual uint64_t get_pipeline_handle(IRenderContext& ctx) = 0;
    virtual size_t gpu_data_size() const = 0;
    virtual void write_gpu_data(void* out, size_t size) const = 0;
};

} // namespace velk

#endif // VELK_RENDER_INTF_MATERIAL_H
