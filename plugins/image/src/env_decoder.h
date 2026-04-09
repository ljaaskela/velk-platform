#ifndef VELK_UI_ENV_DECODER_H
#define VELK_UI_ENV_DECODER_H

#include <velk/ext/object.h>
#include <velk/interface/resource/intf_resource_decoder.h>

#include <velk-ui/plugins/image/plugin.h>

namespace velk::ui::impl {

/**
 * @brief Resource decoder that loads equirectangular HDR images into
 *        `Environment` objects.
 *
 * Registered with the resource store under the name "env". Apps fetch
 * decoded environments via
 * `instance().resource_store().get_resource<IEnvironment>(
 *     "env:app://path/to/file.hdr")`.
 *
 * Supports any format stb_image can load as float (HDR Radiance .hdr,
 * and also regular LDR formats which are promoted to float internally).
 * Output pixel data is stored as RGBA16F (half-float).
 */
class EnvDecoder final : public ::velk::ext::Object<EnvDecoder, IResourceDecoder>
{
public:
    VELK_CLASS_UID(::velk::ui::ClassId::EnvDecoder, "EnvDecoder");

    string_view name() const override { return "env"; }
    IResource::Ptr decode(const IResource::Ptr& inner) const override;
};

} // namespace velk::ui::impl

#endif // VELK_UI_ENV_DECODER_H
