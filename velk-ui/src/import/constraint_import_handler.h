#ifndef VELK_UI_CONSTRAINT_IMPORT_HANDLER_H
#define VELK_UI_CONSTRAINT_IMPORT_HANDLER_H

#include <velk/ext/core_object.h>
#include <velk/interface/intf_importer_extension.h>

#include <velk-ui/types.h>

namespace velk_ui {

dim parse_dim(velk::string_view str);

class ConstraintImportHandler
    : public velk::ext::ObjectCore<ConstraintImportHandler, velk::IImporterExtension>
{
public:
    VELK_CLASS_UID("d0a6f4b5-1c7e-4a9d-bf80-4e5f6a7b8c9e", "ConstraintImportHandler");

    velk::string_view collection_key() const override;
    void process(const velk::IImportData& data, velk::IStore& store,
                 const velk::IImportResolver& resolver) const override;
};

} // namespace velk_ui

#endif // VELK_UI_CONSTRAINT_IMPORT_HANDLER_H
