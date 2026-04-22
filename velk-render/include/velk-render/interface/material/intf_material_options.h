#ifndef VELK_RENDER_INTF_MATERIAL_OPTIONS_H
#define VELK_RENDER_INTF_MATERIAL_OPTIONS_H

#include <velk/interface/intf_metadata.h>

#include <velk-render/render_types.h>

#include <cstdint>

namespace velk {

/// glTF 2.0 alphaMode.
enum class AlphaMode : uint8_t
{
    Opaque,  ///< Alpha channel ignored.
    Mask,    ///< Fragments with alpha < alpha_cutoff are discarded.
    Blend,   ///< Standard alpha blending.
};

/**
 * @brief Pipeline-state options attached to any material.
 *
 * Controls how the pipeline is configured around the material's shading
 * body: alpha mode, cutoff, culling. Values here drive pipeline creation
 * and fragment discard, not the shader's eval function.
 *
 * Attach to a material via `IObjectStorage::add_attachment`. Absence
 * means defaults: opaque, cutoff 0.5, back-face culled (single-sided).
 *
 * `on_options_changed` fires after any PROP write so dependent
 * materials can invalidate their cached pipeline.
 */
class IMaterialOptions : public Interface<IMaterialOptions>
{
public:
    VELK_INTERFACE(
        (PROP, AlphaMode, alpha_mode,   AlphaMode::Opaque),
        (PROP, float,     alpha_cutoff, 0.5f),
        (PROP, CullMode,  cull_mode,    CullMode::Back),
        (PROP, FrontFace, front_face,   FrontFace::Clockwise),
        (PROP, CompareOp, depth_test,   CompareOp::LessEqual),
        (PROP, bool,      depth_write,  true),
        (EVT,  on_options_changed)
    )
};

} // namespace velk

#endif // VELK_RENDER_INTF_MATERIAL_OPTIONS_H
