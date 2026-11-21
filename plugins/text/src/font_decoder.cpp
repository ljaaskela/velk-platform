#include "font_decoder.h"

#include "font.h"

#include <velk/api/velk.h>
#include <velk/interface/resource/intf_resource.h>

namespace velk::ui::impl {

IResource::Ptr FontDecoder::decode(const IResource::Ptr& inner) const
{
    auto* file = interface_cast<IFile>(inner);
    if (!file) {
        VELK_LOG(E, "FontDecoder: Resource must implement IFile.");
        return nullptr;
    }

    auto font = instance().create<IFontInternal>(ClassId::Font);
    if (!font) {
        return nullptr;
    }

    vector<uint8_t> bytes;
    if (!succeeded(file->read(bytes))) {
        bytes.clear();
    }
    if (!font->init(inner->uri(), std::move(bytes))) {
        VELK_LOG(E, "FontDecoder: Cannot load '%s' as a font", file->uri());
    }

    return interface_pointer_cast<IResource>(font);
}

} // namespace velk::ui::impl
