#include "light_type_extension.h"

#include "enum_names.h"

#include <velk/api/any.h>
#include <velk-render/interface/intf_light.h>

namespace velk::ui {

array_view<Uid> LightTypeExtension::supported_types() const
{
    static const Uid types[] = {
        type_uid<LightType>()
    };
    return {types, 1};
}

IAny::Ptr LightTypeExtension::deserialize(Uid uid, const IImportData& data) const
{
    if (uid == type_uid<LightType>()) {
        if (data.kind() == IImportData::Kind::String) {
            auto s = data.as_string();
            if (s == "directional") return Any<LightType>(LightType::Directional);
            if (s == "point")       return Any<LightType>(LightType::Point);
            if (s == "spot")        return Any<LightType>(LightType::Spot);
            if (s == "area")        return Any<LightType>(LightType::Area);
        }
        if (data.kind() == IImportData::Kind::Number) {
            return Any<LightType>(static_cast<LightType>(static_cast<uint8_t>(data.as_number())));
        }
    }

    return {};
}

ReturnValue LightTypeExtension::serialize(Uid uid, const IAny& value, IExportData& out) const
{
    static const EnumName<LightType> names[] = {{LightType::Directional, "directional"},
                                                {LightType::Point, "point"},
                                                {LightType::Spot, "spot"},
                                                {LightType::Area, "area"}};
    return uid == type_uid<LightType>() ? write_enum(value, uid, names, out) : ReturnValue::Fail;
}

} // namespace velk::ui
