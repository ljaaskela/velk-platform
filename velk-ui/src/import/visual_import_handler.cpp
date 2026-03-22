#include "visual_import_handler.h"

#include <velk/api/state.h>
#include <velk/api/velk.h>
#include <velk/interface/intf_object_storage.h>

#include <velk-ui/interface/intf_visual.h>
#include <velk-ui/plugin.h>

namespace velk_ui {

velk::string_view VisualImportHandler::collection_key() const
{
    return "ui-visuals";
}

void VisualImportHandler::process(const velk::IImportData& data, velk::IStore&,
                                  const velk::IImportResolver& resolver) const
{
    auto& velk = velk::instance();

    for (size_t i = 0; i < data.count(); ++i) {
        auto& entry = data.at(i);

        auto target_id = entry.find("target").as_string();
        if (target_id.empty()) {
            continue;
        }

        auto target_obj = resolver.resolve(target_id);
        if (!target_obj) {
            VELK_LOG(W,
                     "ui-visuals: target '%.*s' not found",
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

        if (type_str == "Rect") {
            auto visual_obj = velk.create<velk::IObject>(ClassId::Visual::Rect);
            if (!visual_obj) {
                continue;
            }

            auto& color_data = props.find("color");
            if (!color_data.is_null()) {
                velk::write_state<IVisual>(visual_obj, [&](IVisual::State& s) {
                    auto& r = color_data.find("r");
                    auto& g = color_data.find("g");
                    auto& b = color_data.find("b");
                    auto& a = color_data.find("a");
                    if (!r.is_null()) {
                        s.color.r = static_cast<float>(r.as_number());
                    }
                    if (!g.is_null()) {
                        s.color.g = static_cast<float>(g.as_number());
                    }
                    if (!b.is_null()) {
                        s.color.b = static_cast<float>(b.as_number());
                    }
                    if (!a.is_null()) {
                        s.color.a = static_cast<float>(a.as_number());
                    }
                });
            }

            auto att = velk::interface_pointer_cast<velk::IInterface>(visual_obj);
            storage->add_attachment(att);

        } else {
            VELK_LOG(
                W, "ui-visuals: unknown type '%.*s'", static_cast<int>(type_str.size()), type_str.data());
        }
    }
}

} // namespace velk_ui
