#ifndef VELK_RENDER_EXT_MATERIAL_H
#define VELK_RENDER_EXT_MATERIAL_H

#include <velk-render/ext/gpu_resource.h>
#include <velk-render/interface/intf_draw_data.h>
#include <velk-render/interface/intf_material_internal.h>
#include <velk-render/interface/intf_render_context.h>

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
};

} // namespace velk::ext

#endif // VELK_RENDER_EXT_MATERIAL_H
