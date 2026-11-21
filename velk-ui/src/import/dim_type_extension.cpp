#include "dim_type_extension.h"

#include <velk/api/any.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace velk::ui {

dim parse_dim(string_view str)
{
    std::string s(str.data(), str.size());

    if (s.empty()) {
        return dim::none();
    }
    if (s == "auto") {
        return dim::auto_size();
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

array_view<Uid> DimTypeExtension::supported_types() const
{
    static const Uid types[] = { type_uid<dim>() };
    return { types, 1 };
}

IAny::Ptr DimTypeExtension::deserialize(Uid, const IImportData& data) const
{
    if (data.kind() == IImportData::Kind::String) {
        return Any<dim>(parse_dim(data.as_string()));
    }
    if (data.kind() == IImportData::Kind::Number) {
        return Any<dim>(dim::px(static_cast<float>(data.as_number())));
    }
    return {};
}

ReturnValue DimTypeExtension::serialize(Uid, const IAny& value, IExportData& out) const
{
    dim d{};
    if (failed(value.get_data(&d, sizeof(d), type_uid<dim>()))) {
        return ReturnValue::Fail;
    }
    // The strings parse_dim reads: "auto", "240px", "50%", and "" for none.
    char buf[32];
    switch (d.unit) {
    case DimUnit::Auto:
        out.text("auto");
        break;
    case DimUnit::Px:
        std::snprintf(buf, sizeof(buf), "%gpx", static_cast<double>(d.value));
        out.text(string_view(buf, std::strlen(buf)));
        break;
    case DimUnit::Pct:
        std::snprintf(buf, sizeof(buf), "%g%%", static_cast<double>(d.value * 100.f));
        out.text(string_view(buf, std::strlen(buf)));
        break;
    default:
        out.text("");
        break;
    }
    return ReturnValue::Success;
}

} // namespace velk::ui
