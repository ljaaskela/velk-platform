#ifndef VELK_UI_INTF_TREE_VIEW_H
#define VELK_UI_INTF_TREE_VIEW_H

#include <velk/api/math_types.h>
#include <velk/api/object_ref.h>
#include <velk/interface/intf_metadata.h>
#include <velk/string.h>

namespace velk::ui {

/**
 * @brief A tree of rows: one per shown item of an item source, indented by depth.
 *
 * `source` is an IItemSource (velk_data), for example a HierarchySource. An item's children
 * show while it is expanded; the top level starts expanded. Like a ListView, the view is as tall
 * as its shown rows and creates rows only where they show. Each row is indented `indent` per
 * level after a marker column of `indent`, where the view draws + or - for an item with
 * children; a click there expands or collapses it, a click elsewhere selects. The selected row
 * has a background in the visual's `color`.
 *
 * `row_template` is a store file whose root has a velk-ui.ListRow attachment with id `row`;
 * empty uses the built-in one, a text label bound to `row.item.label`.
 */
class ITreeView : public Interface<ITreeView>
{
public:
    VELK_INTERFACE(
        (PROP, ObjectRef, source, {}),        ///< The IItemSource to show.
        (PROP, string, row_template, {}),     ///< Store file of a row; empty for the built-in one.
        (PROP, float, row_height, 22.f),      ///< Height of a row, in pixels.
        (PROP, float, indent, 16.f),          ///< Indentation per level and marker width, in pixels.
        (PROP, ObjectRef, selected, {}),      ///< The selected item, or none.
        (PROP, ::velk::color, marker_color, (::velk::color{0.75f, 0.77f, 0.8f, 1.f})) ///< Color of + and -.
    )
};

} // namespace velk::ui

#endif // VELK_UI_INTF_TREE_VIEW_H
