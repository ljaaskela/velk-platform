#ifndef VELK_RENDER_EXT_MATERIAL_H
#define VELK_RENDER_EXT_MATERIAL_H

#include <velk/api/velk.h>

#include <velk-render/ext/gpu_resource.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_draw_data.h>
#include <velk-render/interface/intf_material_internal.h>
#include <velk-render/interface/intf_program_data_buffer.h>
#include <velk-render/interface/intf_render_context.h>
#include <velk-render/plugin.h>

namespace velk::ext {

/**
 * @brief CRTP base for application-defined materials.
 *
 * Use this when you write both the shader and the material class, and
 * control the GPU data layout directly. Provides pipeline handle storage
 * and lazy compilation via ensure_pipeline(). Derived classes implement
 * gpu_data_size() and write_gpu_data().
 *
 * For dynamic materials where inputs are discovered from the shader,
 * use ShaderMaterial instead.
 *
 * Usage:
 *   class MyMaterial : public velk::ext::Material<MyMaterial, IMyProps>
 *   {
 *       VELK_CLASS_UID(...);
 *       size_t gpu_data_size() const override { return sizeof(MyParams); }
 *       void write_gpu_data(void* out, size_t size) const override { ... }
 *   };
 */
template <class T, class... Extra>
class Material : public GpuResource<T, IMaterialInternal, IDrawData, Extra...>
{
public:
    uint64_t get_pipeline_handle(IRenderContext&) override { return handle_; }
    void set_pipeline_handle(uint64_t handle) override { handle_ = handle; }

    // IMaterial defaults. Concrete materials override to provide real
    // eval + vertex sources; unimplemented materials return empty and
    // the composers fall back to the (transitional) IRasterShader /
    // IShaderSnippet path until every material has migrated.
    string_view get_eval_src() const override { return {}; }
    string_view get_eval_fn_name() const override { return {}; }
    string_view get_vertex_src() const override { return {}; }
    void register_eval_includes(IRenderContext&) const override {}
    float get_forward_discard_threshold() const override { return 0.001f; }
    float get_deferred_discard_threshold() const override { return 0.5f; }

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
    ::velk::IBuffer::Ptr get_data_buffer() override
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
        data_buffer_->write(sz, [this, &ok](void* dst, size_t n) {
            ok = this->write_draw_data(dst, n) == ReturnValue::Success;
        });
        return ok ? data_buffer_ : nullptr;
    }

protected:
    /**
     * @brief Compiles shaders from source on first call and caches the pipeline handle.
     * @param fragment_source Fragment shader GLSL source.
     * @param vertex_source  Vertex shader GLSL source (empty = use registered default).
     */
    uint64_t ensure_pipeline(IRenderContext& ctx, string_view fragment_source, string_view vertex_source = {})
    {
        if (!has_pipeline_handle()) {
            handle_ = ctx.compile_pipeline(fragment_source, vertex_source);
        }
        return handle_;
    }

    /**
     * @brief Creates a pipeline from pre-compiled shaders on first call and caches the handle.
     * @param fragment Compiled fragment shader (nullptr = use registered default).
     * @param vertex   Compiled vertex shader (nullptr = use registered default).
     */
    uint64_t ensure_pipeline(IRenderContext& ctx, const IShader::Ptr& fragment,
                             const IShader::Ptr& vertex = {})
    {
        if (!has_pipeline_handle()) {
            handle_ = ctx.create_pipeline(vertex, fragment);
        }
        return handle_;
    }

    /** @brief Returns true if the pipeline handle has already been created. */
    bool has_pipeline_handle() const { return handle_ != 0; }

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
    uint64_t handle_{};
    ::velk::IProgramDataBuffer::Ptr data_buffer_;
};

} // namespace velk::ext

#endif // VELK_RENDER_EXT_MATERIAL_H
