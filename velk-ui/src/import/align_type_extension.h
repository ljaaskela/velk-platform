#ifndef VELK_UI_ALIGN_TYPE_EXTENSION_H
#define VELK_UI_ALIGN_TYPE_EXTENSION_H

#include <velk/ext/core_object.h>
#include <velk/interface/intf_importer_extension.h>

#include <velk-ui/plugin.h>

namespace velk::ui {

class AlignTypeExtension
    : public ::velk::ext::ObjectCore<AlignTypeExtension, IImporterTypeExtension>
{
public:
    VELK_CLASS_UID(ClassId::Import::AlignTypeExtension, "AlignTypeExtension");

    array_view<Uid> supported_types() const override;
    IAny::Ptr deserialize(Uid type_uid, const IImportData& data) const override;
};

} // namespace velk::ui

#endif // VELK_UI_ALIGN_TYPE_EXTENSION_H
