#ifndef VELK_UI_ENUM_NAMES_H
#define VELK_UI_ENUM_NAMES_H

#include <velk/interface/intf_any.h>
#include <velk/interface/intf_exporter_extension.h>
#include <velk/string_view.h>

#include <cstddef>
#include <cstdint>

namespace velk::ui {

template <class E>
struct EnumName
{
    E value;
    string_view name;
};

/** @brief Writes an enum by the name its type extension reads, or as a number when it has none. */
template <class E, size_t N>
ReturnValue write_enum(const IAny& value, Uid type, const EnumName<E> (&names)[N], IExportData& out)
{
    E v{};
    if (failed(value.get_data(&v, sizeof(E), type))) {
        return ReturnValue::Fail;
    }
    for (auto& n : names) {
        if (n.value == v) {
            out.text(n.name);
            return ReturnValue::Success;
        }
    }
    out.integer(static_cast<int64_t>(v));
    return ReturnValue::Success;
}

} // namespace velk::ui

#endif // VELK_UI_ENUM_NAMES_H
