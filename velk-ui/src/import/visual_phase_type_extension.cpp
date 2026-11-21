#include "visual_phase_type_extension.h"

#include "enum_names.h"

#include <velk/api/any.h>
#include <velk-scene/interface/intf_visual.h>

namespace velk::ui {

array_view<Uid> VisualPhaseTypeExtension::supported_types() const
{
    static const Uid types[] = {
        type_uid<VisualPhase>()
    };
    return {types, 1};
}

IAny::Ptr VisualPhaseTypeExtension::deserialize(Uid uid, const IImportData& data) const
{
    if (uid == type_uid<VisualPhase>()) {
        if (data.kind() == IImportData::Kind::String) {
            auto s = data.as_string();
            if (s == "before_children") return Any<VisualPhase>(VisualPhase::BeforeChildren);
            if (s == "after_children")  return Any<VisualPhase>(VisualPhase::AfterChildren);
        }
        if (data.kind() == IImportData::Kind::Number) {
            return Any<VisualPhase>(static_cast<VisualPhase>(static_cast<uint8_t>(data.as_number())));
        }
    }

    return {};
}

ReturnValue VisualPhaseTypeExtension::serialize(Uid uid, const IAny& value, IExportData& out) const
{
    static const EnumName<VisualPhase> names[] = {{VisualPhase::BeforeChildren, "before_children"},
                                                  {VisualPhase::AfterChildren, "after_children"}};
    return uid == type_uid<VisualPhase>() ? write_enum(value, uid, names, out) : ReturnValue::Fail;
}

} // namespace velk::ui
