#ifndef VELK_UI_DIM_TYPE_EXTENSION_H
#define VELK_UI_DIM_TYPE_EXTENSION_H

#include <velk/ext/core_object.h>
#include <velk/interface/intf_importer_extension.h>
#include <velk/string.h>

#include <velk-ui/plugin.h>
#include <velk-ui/types.h>

namespace velk::ui {

dim parse_dim(string_view str);

class DimTypeExtension
    : public ::velk::ext::ObjectCore<DimTypeExtension, IImporterTypeExtension>
{
public:
    VELK_CLASS_UID(ClassId::Import::DimTypeExtension, "DimTypeExtension");

    array_view<Uid> supported_types() const override;
    IAny::Ptr deserialize(Uid type_uid, const IImportData& data) const override;
};

} // namespace velk::ui

#endif // VELK_UI_DIM_TYPE_EXTENSION_H
