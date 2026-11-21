#ifndef VELK_UI_VISUAL_PHASE_TYPE_EXTENSION_H
#define VELK_UI_VISUAL_PHASE_TYPE_EXTENSION_H

#include <velk/ext/core_object.h>
#include <velk/interface/intf_exporter_extension.h>
#include <velk/interface/intf_importer_extension.h>

#include <velk-ui/plugin.h>

namespace velk::ui {

class VisualPhaseTypeExtension
    : public ::velk::ext::ObjectCore<VisualPhaseTypeExtension, IImporterTypeExtension, IExporterTypeExtension>
{
public:
    VELK_CLASS_UID(ClassId::Import::VisualPhaseTypeExtension, "VisualPhaseTypeExtension");

    array_view<Uid> supported_types() const override;
    IAny::Ptr deserialize(Uid type_uid, const IImportData& data) const override;
    ReturnValue serialize(Uid type_uid, const IAny& value, IExportData& out) const override;
};

} // namespace velk::ui

#endif // VELK_UI_VISUAL_PHASE_TYPE_EXTENSION_H
