#ifndef VELK_UI_INTF_MATERIAL_H
#define VELK_UI_INTF_MATERIAL_H

#include <velk/interface/intf_metadata.h>

#include <cstddef>
#include <cstdint>

namespace velk_ui {

class IRenderContext; // forward declaration

/**
 * @brief Interface for custom materials that override the default color fill.
 *
 * A material defines how a visual's geometry is shaded. When a visual's `paint`
 * property references an IMaterial, the renderer uses the material's pipeline
 * instead of the visual's `color` property.
 *
 * Every material provides a pipeline handle and GPU data blob. The renderer
 * writes the GPU data after the DrawDataHeader in the frame buffer. The
 * material's shader reads it via buffer_reference from the draw data pointer.
 */
class IMaterial : public velk::Interface<IMaterial>
{
public:
    /**
     * @brief Returns the pipeline handle for this material's shader program.
     *
     * The render context is provided so that materials can lazily compile
     * and register their pipeline on first use.
     * Returns 0 if no pipeline is available.
     */
    virtual uint64_t get_pipeline_handle(IRenderContext& ctx) = 0;

    /**
     * @brief Returns the size in bytes of this material's GPU data.
     *
     * The renderer allocates this many bytes after the DrawDataHeader in
     * the staging buffer and passes the pointer to write_gpu_data().
     * Returns 0 if no material data is needed.
     */
    virtual size_t gpu_data_size() const = 0;

    /**
     * @brief Writes material-specific GPU data into the provided buffer.
     *
     * Called with a pointer to exactly gpu_data_size() bytes. The data
     * layout must match what the material's shader expects to read after
     * the standard DrawDataHeader.
     */
    virtual void write_gpu_data(void* out, size_t size) const = 0;
};

} // namespace velk_ui

#endif // VELK_UI_INTF_MATERIAL_H
