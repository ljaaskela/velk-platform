#ifndef VELK_RENDER_EXT_MATERIAL_H
#define VELK_RENDER_EXT_MATERIAL_H

#include <velk/api/velk.h>

#include <velk/api/event.h>
#include <velk/api/state.h>

#include <velk-render/ext/element_vertex.h>
#include <velk-render/ext/gpu_resource.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_draw_data.h>
#include <velk-render/interface/intf_shader_source.h>
#include <velk-render/interface/material/intf_material_internal.h>
#include <velk-render/interface/material/intf_material_options.h>
#include <velk-render/interface/intf_program_data_buffer.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/plugin.h>

namespace velk::ext {

/**
 * @brief CRTP base for application-defined materials.
 *
 * Use this when you write both the shader and the material class, and
 * control the GPU data layout directly. Provides pipeline handle
 * storage, lazy compilation via ensure_pipeline() (for the full-fragment
 * path), and `IMaterialOptions` subscription so option writes invalidate
 * the cached pipeline. Derived classes override IDrawData's
 * get_draw_data_size() / write_draw_data(), and IShaderSource's
 * get_source / get_fn_name to return their eval body and (optionally)
 * a custom vertex shader. The framework composes forward / deferred /
 * RT drivers from the eval body.
 *
 * For dynamic materials where inputs are discovered from the shader,
 * use ShaderMaterial instead.
 *
 * Usage:
 *   class MyMaterial : public velk::ext::Material<MyMaterial, IMyProps>
 *   {
 *       VELK_CLASS_UID(...);
 *       size_t       get_draw_data_size() const override { return sizeof(MyParams); }
 *       ReturnValue  write_draw_data(void* out, size_t size,
 *                                    ITextureResolver*) const override { ... }
 *       string_view  get_source(string_view role) const override {
 *           if (role == shader_role::kEval) return my_eval_glsl;
 *           return Base::get_source(role);
 *       }
 *       string_view  get_fn_name(string_view role) const override {
 *           return role == shader_role::kEval ? "velk_eval_my" : string_view{};
 *       }
 *   };
 */
template <class T, class... Extra>
class Material : public GpuResource<T, IMaterialInternal, IDrawData,
                                    ::velk::IShaderSource, Extra...>
{
    using Base = GpuResource<T, IMaterialInternal, IDrawData,
                             ::velk::IShaderSource, Extra...>;

public:
    // Pipeline handle is stored as the resource's primary handle (key
    // GpuResourceKey::Default) by `ext::GpuResource`. The IMaterialInternal
    // accessors below are thin wrappers for ergonomic call sites.
    uint64_t get_pipeline_handle(IRenderContext&) override
    {
        return this->get_gpu_handle(GpuResourceKey::Default);
    }
    void set_pipeline_handle(uint64_t handle) override
    {
        this->set_gpu_handle(GpuResourceKey::Default, handle);
    }

    /// Subscribe / unsubscribe to IMaterialOptions attachments so option
    /// writes propagate into pipeline invalidation without polling.
    ReturnValue add_attachment(const IInterface::Ptr& attachment) override
    {
        auto rv = Base::add_attachment(attachment);
        if (succeeded(rv)) {
            if (auto* opts = interface_cast<IMaterialOptions>(attachment)) {
                bind_options_subscription(opts);
            }
        }
        return rv;
    }

    ReturnValue remove_attachment(const IInterface::Ptr& attachment) override
    {
        bool was_options = interface_cast<IMaterialOptions>(attachment) != nullptr;
        auto rv = Base::remove_attachment(attachment);
        if (succeeded(rv) && was_options) {
            constexpr AttachmentQuery q{IMaterialOptions::UID, {}};
            auto next = this->find_attachment(q, Resolve::Existing);
            bind_options_subscription(interface_cast<IMaterialOptions>(next));
        }
        return rv;
    }

    // IShaderSource defaults. Concrete materials override get_source /
    // get_fn_name to return their eval / vertex / fragment bodies; the
    // shared element_vertex_src is the default for the kVertex role so
    // simple materials don't repeat the boilerplate.
    string_view get_source(string_view role) const override
    {
        if (role == ::velk::shader_role::kVertex) return ext::element_vertex_src;
        return {};
    }
    string_view get_fn_name(string_view) const override { return {}; }
    uint64_t get_pipeline_key() const override { return 0; }
    void register_includes(IRenderContext&) const override {}

    /// Framework-level discard thresholds derived from the attached
    /// IMaterialOptions (if any). Mask mode → opts.alpha_cutoff; Blend
    /// mode → 0 (no discard, blending handles alpha); Opaque → a tiny
    /// epsilon to drop fully-transparent fragments.
    float get_forward_discard_threshold() const override
    {
        return discard_threshold_from_options(0.001f);
    }
    float get_deferred_discard_threshold() const override
    {
        return discard_threshold_from_options(0.5f);
    }
    vector<ISurface*> get_textures() const override { return {}; }

protected:
    float discard_threshold_from_options(float opaque_default) const
    {
        constexpr AttachmentQuery q{IMaterialOptions::UID, {}};
        auto att = const_cast<Material*>(this)->find_attachment(q, Resolve::Existing);
        if (auto r = ::velk::read_state<::velk::IMaterialOptions>(att)) {
            switch (r->alpha_mode) {
            case ::velk::AlphaMode::Mask:  return r->alpha_cutoff;
            case ::velk::AlphaMode::Blend: return 0.0f;
            case ::velk::AlphaMode::Opaque:
            default:                       break;
            }
        }
        return opaque_default;
    }

public:

    /**
     * @brief Default persistent-buffer implementation.
     *
     * Serialises the material's current draw data via the derived
     * class's `write_draw_data` into an owned `ProgramDataBuffer`. The
     * buffer diffs the bytes and only flags dirty on actual change, so
     * unchanged materials skip re-upload. The returned pointer is
     * stable across frames; its GPU address stays valid until the
     * material (or a capturing frame) drops its last reference.
     *
     * Materials with no draw data return nullptr, which keeps the
     * renderer on the frame-scratch fallback path.
     */
    ::velk::IBuffer::Ptr get_data_buffer(::velk::ITextureResolver* resolver = nullptr) override
    {
        size_t sz = this->get_draw_data_size();
        if (sz == 0) {
            return nullptr;
        }
        if (!data_buffer_) {
            // Allocate through the type registry so instantiation can
            // use the framework's object hive for fast reuse.
            data_buffer_ = ::velk::instance().create<::velk::IProgramDataBuffer>(
                ::velk::ClassId::ProgramDataBuffer);
            if (!data_buffer_) {
                return nullptr;
            }
        }
        bool ok = true;
        data_buffer_->write(sz, [this, &ok, resolver](void* dst, size_t n) {
            ok = this->write_draw_data(dst, n, resolver) == ReturnValue::Success;
        });
        return ok ? data_buffer_ : nullptr;
    }

protected:
    /** @brief Returns true if the pipeline handle has already been created. */
    bool has_pipeline_handle() const
    {
        return this->get_gpu_handle(GpuResourceKey::Default) != 0;
    }

    /**
     * @brief Writes a typed material params struct into the GPU data buffer.
     *
     * Zero-initializes the struct, then calls @p fn to fill it.
     * Returns ReturnValue::Fail if @p size does not match sizeof(Params).
     *
     * @param out  Destination buffer (after DrawDataHeader).
     * @param size Buffer size in bytes.
     * @param fn   Callable that receives Params& to populate.
     */
    template <typename Params, typename Fn>
    static ReturnValue set_material(void* out, size_t size, Fn&& fn)
    {
        if (size == sizeof(Params)) {
            auto& p = *static_cast<Params*>(out);
            p = {};
            fn(p);
            return ReturnValue::Success;
        }
        return ReturnValue::Fail;
    }

private:
    void bind_options_subscription(IMaterialOptions* opts)
    {
        if (opts) {
            options_sub_ = ScopedHandler(opts->on_options_changed(),
                [this]() { this->set_gpu_handle(GpuResourceKey::Default, 0); });
        } else {
            options_sub_.reset();
        }
    }

    ::velk::IProgramDataBuffer::Ptr data_buffer_;
    ScopedHandler options_sub_;
};

} // namespace velk::ext

#endif // VELK_RENDER_EXT_MATERIAL_H
