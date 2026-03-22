#include "constraint_import_handler.h"

#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/interface/intf_object_storage.h>

#include <string>
#include <velk-ui/interface/constraint/intf_fixed_size.h>
#include <velk-ui/interface/constraint/intf_stack.h>
#include <velk-ui/plugin.h>

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

velk::string_view ConstraintImportHandler::collection_key() const
{
    return "ui-constraints";
}

void ConstraintImportHandler::process(const velk::IImportData& data, velk::IStore&,
                                      const velk::IImportResolver& resolver) const
{
    auto& velk = velk::instance();

    for (size_t i = 0; i < data.count(); ++i) {
        auto& entry = data.at(i);

        // Resolve target element
        auto target_id = entry.find("target").as_string();
        if (target_id.empty()) {
            continue;
        }

        auto target_obj = resolver.resolve(target_id);
        if (!target_obj) {
            VELK_LOG(W,
                     "ui-constraints: target '%.*s' not found",
                     static_cast<int>(target_id.size()),
                     target_id.data());
            continue;
        }

        auto type_str = entry.find("type").as_string();
        if (type_str.empty()) {
            continue;
        }

        auto* storage = velk::interface_cast<velk::IObjectStorage>(target_obj);
        if (!storage) {
            continue;
        }

        auto& props = entry.find("properties");

        if (type_str == "Stack") {
            auto constraint_obj = velk.create<velk::IObject>(ClassId::Constraint::Stack);
            if (!constraint_obj) {
                continue;
            }

            // Parse stack properties
            auto& axis_data = props.find("axis");
            auto& spacing_data = props.find("spacing");

            if (!axis_data.is_null() || !spacing_data.is_null()) {
                velk::write_state<IStack>(constraint_obj, [&](IStack::State& s) {
                    if (!axis_data.is_null()) {
                        s.axis = static_cast<uint8_t>(axis_data.as_number());
                    }
                    if (!spacing_data.is_null()) {
                        s.spacing = static_cast<float>(spacing_data.as_number());
                    }
                });
            }

            auto att = velk::interface_pointer_cast<velk::IInterface>(constraint_obj);
            storage->add_attachment(att);

        } else if (type_str == "FixedSize") {
            auto constraint_obj = velk.create<velk::IObject>(ClassId::Constraint::FixedSize);
            if (!constraint_obj) {
                continue;
            }

            // Parse fixed size properties
            auto& w_data = props.find("width");
            auto& h_data = props.find("height");

            if (!w_data.is_null() || !h_data.is_null()) {
                velk::write_state<IFixedSize>(constraint_obj, [&](IFixedSize::State& s) {
                    if (!w_data.is_null()) {
                        if (w_data.kind() == velk::IImportData::Kind::String) {
                            s.width = parse_dim(w_data.as_string());
                        } else {
                            s.width = dim::px(static_cast<float>(w_data.as_number()));
                        }
                    }
                    if (!h_data.is_null()) {
                        if (h_data.kind() == velk::IImportData::Kind::String) {
                            s.height = parse_dim(h_data.as_string());
                        } else {
                            s.height = dim::px(static_cast<float>(h_data.as_number()));
                        }
                    }
                });
            }

            auto att = velk::interface_pointer_cast<velk::IInterface>(constraint_obj);
            storage->add_attachment(att);

        } else {
            VELK_LOG(
                W, "ui-constraints: unknown type '%.*s'", static_cast<int>(type_str.size()), type_str.data());
        }
    }
}

} // namespace velk_ui
