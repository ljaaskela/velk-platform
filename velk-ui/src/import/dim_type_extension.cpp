#include "dim_type_extension.h"

#include <velk/api/any.h>

#include <string>

namespace velk_ui {

dim parse_dim(velk::string_view str)
{
    std::string s(str.data(), str.size());

    if (s.empty()) {
        return dim::none();
    }

    // Check for "px" suffix
    if (s.size() > 2 && s.compare(s.size() - 2, 2, "px") == 0) {
        float val = std::stof(s.substr(0, s.size() - 2));
        return dim::px(val);
    }

    // Check for "%" suffix
    if (s.back() == '%') {
        float val = std::stof(s.substr(0, s.size() - 1));
        return dim::pct(val / 100.f);
    }

    // Bare number treated as px
    float val = std::stof(s);
    return dim::px(val);
}

velk::array_view<velk::Uid> DimTypeExtension::supported_types() const
{
    static const velk::Uid types[] = { velk::type_uid<dim>() };
    return { types, 1 };
}

velk::IAny::Ptr DimTypeExtension::deserialize(velk::Uid, const velk::IImportData& data) const
{
    if (data.kind() == velk::IImportData::Kind::String) {
        return velk::Any<dim>(parse_dim(data.as_string()));
    }
    if (data.kind() == velk::IImportData::Kind::Number) {
        return velk::Any<dim>(dim::px(static_cast<float>(data.as_number())));
    }
    return {};
}

} // namespace velk_ui
