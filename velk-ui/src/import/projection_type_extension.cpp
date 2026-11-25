#include "projection_type_extension.h"

#include <velk/api/any.h>
#include <velk-render/interface/intf_camera.h>

namespace velk::ui {

array_view<Uid> ProjectionTypeExtension::supported_types() const
{
    static const Uid types[] = {
        type_uid<Projection>(),
        type_uid<SunPass>()
    };
    return {types, 2};
}

IAny::Ptr ProjectionTypeExtension::deserialize(Uid uid, const IImportData& data) const
{
    if (uid == type_uid<Projection>()) {
        if (data.kind() == IImportData::Kind::String) {
            auto s = data.as_string();
            if (s == "ortho")       return Any<Projection>(Projection::Ortho);
            if (s == "perspective") return Any<Projection>(Projection::Perspective);
        }
        if (data.kind() == IImportData::Kind::Number) {
            return Any<Projection>(static_cast<Projection>(static_cast<uint8_t>(data.as_number())));
        }
    }
    if (uid == type_uid<SunPass>()) {
        if (data.kind() == IImportData::Kind::String) {
            auto s = data.as_string();
            if (s == "off")  return Any<SunPass>(SunPass::Off);
            if (s == "hard") return Any<SunPass>(SunPass::Hard);
            if (s == "soft") return Any<SunPass>(SunPass::Soft);
        }
        if (data.kind() == IImportData::Kind::Number) {
            const double n = data.as_number();
            if (n == 0.0) return Any<SunPass>(SunPass::Off);
            if (n == 1.0) return Any<SunPass>(SunPass::Hard);
            if (n == 2.0) return Any<SunPass>(SunPass::Soft);
        }
    }

    return {};
}

} // namespace velk::ui
