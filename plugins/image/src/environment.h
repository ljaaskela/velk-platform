#ifndef VELK_UI_ENV_IMPL_H
#define VELK_UI_ENV_IMPL_H

#include <velk/string.h>
#include <velk/vector.h>

#include <velk-render/ext/gpu_resource.h>
#include <velk-render/interface/intf_buffer.h>
#include <velk-render/interface/intf_surface.h>
#include <velk-scene/interface/intf_environment.h>
#include <velk-ui/plugins/image/plugin.h>

#include <cstdint>

namespace velk::ui::impl {

class IEnvironmentInternal : public Interface<IEnvironmentInternal, IEnvironment>
{
public:
    virtual void init(string_view uri, int width, int height, vector<uint8_t> pixels) = 0;
};

/**
 * @brief Decoded equirectangular HDR environment map.
 *
 * Implements both `IEnvironment` (resource metadata + env properties)
 * and `ISurface` + `IBuffer` (the GPU-uploadable HDR image). Pixel data is stored
 * as RGBA16F (half-float) for GPU efficiency.
 */
class Environment final : public ::velk::ext::GpuResource<Environment, IEnvironmentInternal, ISurface, IBuffer>
{
public:
    VELK_CLASS_UID(::velk::ui::ClassId::Environment, "Environment");

    GpuResourceType get_type() const override { return GpuResourceType::Texture; }

    Environment() = default;

    /// Initializes from decoded half-float pixel data.
    void init(string_view uri, int width, int height, vector<uint8_t> pixels) override;

    // IResource
    string_view uri() const override { return uri_; }
    bool exists() const override { return width_ > 0 && height_ > 0; }
    int64_t size() const override { return static_cast<int64_t>(pixels_.size()); }
    bool is_persistent() const override { return persistent_; }
    void set_persistent(bool value) override { persistent_ = value; }

    // IBuffer
    size_t get_data_size() const override { return pixels_.size(); }
    const uint8_t* get_data() const override
    {
        return pixels_.empty() ? nullptr : pixels_.data();
    }
    bool is_dirty() const override { return dirty_; }
    void clear_dirty() override { dirty_ = false; }

    // ISurface
    uvec2 get_dimensions() const override { return {static_cast<uint32_t>(width_), static_cast<uint32_t>(height_)}; }
    PixelFormat format() const override { return PixelFormat::RGBA16F; }

    // IEnvironment
    IMaterial::Ptr get_material() const override { return material_; }

private:
    void init_material();
    string uri_;
    int width_{};
    int height_{};
    vector<uint8_t> pixels_; // RGBA16F half-float data
    IMaterial::Ptr material_;
    bool dirty_{};
    bool persistent_{};
};

} // namespace velk::ui::impl

#endif // VELK_UI_ENV_IMPL_H
