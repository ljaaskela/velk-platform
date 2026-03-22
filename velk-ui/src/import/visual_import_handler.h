#ifndef VELK_UI_VISUAL_IMPORT_HANDLER_H
#define VELK_UI_VISUAL_IMPORT_HANDLER_H

#include <velk/ext/core_object.h>
#include <velk/interface/intf_importer_extension.h>

namespace velk_ui {

/**
 * @brief Import handler for the "ui-visuals" JSON collection.
 *
 * Creates IVisual attachments (e.g. RectVisual) on elements from JSON data,
 * following the same pattern as ConstraintImportHandler.
 */
class VisualImportHandler : public velk::ext::ObjectCore<VisualImportHandler, velk::IImporterExtension>
{
public:
    VELK_CLASS_UID("f1a2b3c4-d5e6-4f78-9a0b-c1d2e3f4a5b6", "VisualImportHandler");

    velk::string_view collection_key() const override;
    void process(const velk::IImportData& data, velk::IStore& store,
                 const velk::IImportResolver& resolver) const override;
};

} // namespace velk_ui

#endif // VELK_UI_VISUAL_IMPORT_HANDLER_H
