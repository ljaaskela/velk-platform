#ifndef VELK_UI_INTF_LIST_ROW_H
#define VELK_UI_INTF_LIST_ROW_H

#include <velk/api/object_ref.h>
#include <velk/interface/intf_metadata.h>

namespace velk::ui {

/**
 * @brief What a list or tree view tells one of its rows.
 *
 * A row template's root carries a ListRow attachment with id `row`. The view writes it when
 * it shows the row or reuses it for another item, and the row's elements bind through it:
 * `{ "bind": "row.item.label" }`.
 */
class IListRow : public Interface<IListRow>
{
public:
    VELK_INTERFACE(
        (PROP, ObjectRef, item, {}),        ///< The item the row shows.
        (PROP, uint32_t, index, 0),         ///< Position among the shown rows.
        (PROP, uint32_t, depth, 0),         ///< Tree depth; 0 at the top level.
        (PROP, bool, selected, false),
        (PROP, bool, expanded, false),
        (PROP, bool, has_children, false)
    )
};

} // namespace velk::ui

#endif // VELK_UI_INTF_LIST_ROW_H
