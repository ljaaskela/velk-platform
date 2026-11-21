#include "align_type_extension.h"

#include <velk/api/any.h>
#include <velk-scene/types.h>

namespace velk::ui {

array_view<Uid> AlignTypeExtension::supported_types() const
{
    static const Uid types[] = {
        type_uid<HAlign>(),
        type_uid<VAlign>(),
        type_uid<Alignment>()
    };
    return { types, 3 };
}

IAny::Ptr AlignTypeExtension::deserialize(Uid uid, const IImportData& data) const
{
    if (uid == type_uid<Alignment>()) {
        if (data.kind() == IImportData::Kind::String) {
            auto s = data.as_string();
            if (s == "start")  return Any<Alignment>(Alignment::Start);
            if (s == "center") return Any<Alignment>(Alignment::Center);
            if (s == "end")    return Any<Alignment>(Alignment::End);
            if (s == "fill")   return Any<Alignment>(Alignment::Fill);
        }
        if (data.kind() == IImportData::Kind::Number) {
            return Any<Alignment>(static_cast<Alignment>(static_cast<uint8_t>(data.as_number())));
        }
    }

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
