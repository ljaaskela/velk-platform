#include "projection_type_extension.h"

#include <velk/api/any.h>
#include <velk-render/interface/intf_camera.h>

namespace velk::ui {

array_view<Uid> ProjectionTypeExtension::supported_types() const
{
    static const Uid types[] = {
        type_uid<Projection>()
    };
    return {types, 1};
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

    return {};
}

} // namespace velk::ui
