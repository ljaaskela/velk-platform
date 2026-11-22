#ifndef VELK_UI_INTF_LIST_VIEW_H
#define VELK_UI_INTF_LIST_VIEW_H

#include <velk/api/object_ref.h>
#include <velk/interface/intf_metadata.h>
#include <velk/string.h>

namespace velk::ui {

/**
 * @brief A list of rows, one per top-level item of an item source.
 *
 * `source` is an IItemSource (velk_data). The view is as tall as all its rows; placed in a
 * ScrollView, it creates rows only for the items in its visible part and reuses them as it
 * scrolls. Each row is an instance of the store file `row_template`, whose root has a
 * velk-ui.ListRow attachment with id `row`. A click selects the item under it; the selected
 * row has a background in the visual's `color`.
 */
class IListView : public Interface<IListView>
{
public:
    VELK_INTERFACE(
        (PROP, ObjectRef, source, {}),       ///< The IItemSource to show.
        (PROP, string, row_template, {}),    ///< Store file of a row, such as "app://ui/row.velk".
        (PROP, float, row_height, 24.f),     ///< Height of a row, in pixels.
        (PROP, ObjectRef, selected, {})      ///< The selected item, or none.
    )
};

} // namespace velk::ui

#endif // VELK_UI_INTF_LIST_VIEW_H
