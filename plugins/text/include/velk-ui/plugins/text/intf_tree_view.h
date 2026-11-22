#ifndef VELK_UI_INTF_TREE_VIEW_H
#define VELK_UI_INTF_TREE_VIEW_H

#include <velk/api/math_types.h>
#include <velk/api/object_ref.h>
#include <velk/interface/intf_metadata.h>
#include <velk/string.h>

namespace velk::ui {

/**
 * @brief A tree of rows from a velk object hierarchy.
 *
 * Shows `source`, an IHierarchy, from its root: one row per shown object, indented by its
 * depth, with its `label_property` as text. Clicking a row selects its object; clicking the
 * +/- in front of a row with children expands or collapses it. The root starts expanded.
 * The rows follow changes to the hierarchy. The text has the visual's `color`; the tree's
 * content size is its rows, so in a ScrollView it scrolls.
 */
class ITreeView : public Interface<ITreeView>
{
public:
    VELK_INTERFACE(
        (PROP, ObjectRef, source, {}),                     ///< The IHierarchy to show.
        (PROP, string, label_property, "label"),           ///< The property of each object shown as its text.
        (PROP, ObjectRef, selected, {}),                   ///< The selected object, or none.
        (PROP, float, row_height, 22.f),                   ///< Height of a row, in pixels.
        (PROP, float, indent, 16.f),                       ///< Indentation per level, in pixels.
        (PROP, float, font_size, 14.f),                    ///< Text size, in pixels.
        (PROP, ::velk::color, selection_color, (::velk::color{0.25f, 0.4f, 0.7f, 1.f})) ///< Background of the selected row.
    )
};

} // namespace velk::ui

#endif // VELK_UI_INTF_TREE_VIEW_H
