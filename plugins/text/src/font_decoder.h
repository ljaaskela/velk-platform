#ifndef VELK_UI_TEXT_FONT_DECODER_H
#define VELK_UI_TEXT_FONT_DECODER_H

#include <velk/ext/object.h>
#include <velk/interface/resource/intf_resource_decoder.h>

#include <velk-ui/plugins/text/plugin.h>

namespace velk::ui::impl {

/**
 * @brief Resource decoder that loads TrueType and OpenType files into `Font` objects.
 *
 * Registered under the name "font":
 * `instance().resource_store().get_resource<IFont>("font:app://fonts/x.ttf")`.
 * A file that cannot be read or opened gives a Font with exists() == false.
 */
class FontDecoder final : public ::velk::ext::Object<FontDecoder, IResourceDecoder>
{
public:
    VELK_CLASS_UID(ClassId::FontDecoder, "FontDecoder");

    string_view name() const override { return "font"; }
    IResource::Ptr decode(const IResource::Ptr& inner) const override;
};

} // namespace velk::ui::impl

#endif // VELK_UI_TEXT_FONT_DECODER_H
