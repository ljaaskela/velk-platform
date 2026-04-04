#include "align_type_extension.h"

#include <velk/api/any.h>
#include <velk-ui/types.h>

namespace velk::ui {

array_view<Uid> AlignTypeExtension::supported_types() const
{
    static const Uid types[] = {
        type_uid<HAlign>(),
        type_uid<VAlign>()
    };
    return { types, 2 };
}

IAny::Ptr AlignTypeExtension::deserialize(Uid uid, const IImportData& data) const
{
    if (uid == type_uid<HAlign>()) {
        if (data.kind() == IImportData::Kind::String) {
            auto s = data.as_string();
            if (s == "left")   return Any<HAlign>(HAlign::Left);
            if (s == "center") return Any<HAlign>(HAlign::Center);
            if (s == "right")  return Any<HAlign>(HAlign::Right);
        }
        if (data.kind() == IImportData::Kind::Number) {
            return Any<HAlign>(static_cast<HAlign>(static_cast<uint8_t>(data.as_number())));
        }
    }

    if (uid == type_uid<VAlign>()) {
        if (data.kind() == IImportData::Kind::String) {
            auto s = data.as_string();
            if (s == "top")    return Any<VAlign>(VAlign::Top);
            if (s == "center") return Any<VAlign>(VAlign::Center);
            if (s == "bottom") return Any<VAlign>(VAlign::Bottom);
        }
        if (data.kind() == IImportData::Kind::Number) {
            return Any<VAlign>(static_cast<VAlign>(static_cast<uint8_t>(data.as_number())));
        }
    }

    return {};
}

} // namespace velk::ui
