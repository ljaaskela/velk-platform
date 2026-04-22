#ifndef VELK_RENDER_API_MATERIAL_MATERIAL_OPTIONS_H
#define VELK_RENDER_API_MATERIAL_MATERIAL_OPTIONS_H

#include <velk/api/object.h>
#include <velk/api/state.h>

#include <velk-render/interface/material/intf_material_options.h>
#include <velk-render/plugin.h>

namespace velk {

/**
 * @brief Convenience wrapper for an IMaterialOptions attachment.
 *
 * Null-safe: default-constructed wrappers return defaults on reads and
 * are no-ops on writes.
 */
class MaterialOptions : public Object
{
public:
    MaterialOptions() = default;
    /** @brief Wraps @p obj if it implements IMaterialOptions, otherwise empty. */
    explicit MaterialOptions(IObject::Ptr obj) : Object(check_object<IMaterialOptions>(obj)) {}
    /** @brief Wraps @p mo. */
    explicit MaterialOptions(IMaterialOptions::Ptr mo) : Object(as_object(mo)) {}
    /** @brief Implicit conversion to the underlying IMaterialOptions pointer. */
    operator IMaterialOptions::Ptr() const { return as_ptr<IMaterialOptions>(); }

    /** @brief Alpha handling: Opaque, Mask, or Blend. */
    AlphaMode get_alpha_mode() const
    {
        return read_state_value<IMaterialOptions>(&IMaterialOptions::State::alpha_mode);
    }
    /** @copybrief get_alpha_mode */
    void set_alpha_mode(AlphaMode v)
    {
        write_state_value<IMaterialOptions>(&IMaterialOptions::State::alpha_mode, v);
    }

    /** @brief Threshold for Mask mode. Fragments with alpha < cutoff are discarded. */
    float get_alpha_cutoff() const
    {
        return read_state_value<IMaterialOptions>(&IMaterialOptions::State::alpha_cutoff);
    }
    /** @copybrief get_alpha_cutoff */
    void set_alpha_cutoff(float v)
    {
        write_state_value<IMaterialOptions>(&IMaterialOptions::State::alpha_cutoff, v);
    }

    /** @brief Face culling mode: Back (default), Front, or None (double-sided). */
    CullMode get_cull_mode() const
    {
        return read_state_value<IMaterialOptions>(&IMaterialOptions::State::cull_mode);
    }
    /** @copybrief get_cull_mode */
    void set_cull_mode(CullMode v)
    {
        write_state_value<IMaterialOptions>(&IMaterialOptions::State::cull_mode, v);
    }

    /** @brief True when cull_mode is None. */
    bool get_double_sided() const { return get_cull_mode() == CullMode::None; }
    /** @brief Sets cull_mode to None (true) or Back (false). Convenience for glTF doubleSided. */
    void set_double_sided(bool v) { set_cull_mode(v ? CullMode::None : CullMode::Back); }

    /** @brief Winding that counts as front. CounterClockwise (right-handed / glTF) by default. */
    FrontFace get_front_face() const
    {
        return read_state_value<IMaterialOptions>(&IMaterialOptions::State::front_face);
    }
    /** @copybrief get_front_face */
    void set_front_face(FrontFace v)
    {
        write_state_value<IMaterialOptions>(&IMaterialOptions::State::front_face, v);
    }

    /** @brief Depth compare op. Disabled turns depth testing off for this material. */
    CompareOp get_depth_test() const
    {
        return read_state_value<IMaterialOptions>(&IMaterialOptions::State::depth_test);
    }
    /** @copybrief get_depth_test */
    void set_depth_test(CompareOp v)
    {
        write_state_value<IMaterialOptions>(&IMaterialOptions::State::depth_test, v);
    }

    /** @brief Whether this material writes to the depth buffer. */
    bool get_depth_write() const
    {
        return read_state_value<IMaterialOptions>(&IMaterialOptions::State::depth_write);
    }
    /** @copybrief get_depth_write */
    void set_depth_write(bool v)
    {
        write_state_value<IMaterialOptions>(&IMaterialOptions::State::depth_write, v);
    }
};

} // namespace velk

#endif // VELK_RENDER_API_MATERIAL_MATERIAL_OPTIONS_H
