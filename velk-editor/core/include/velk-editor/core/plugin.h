#ifndef VELK_EDITOR_CORE_PLUGIN_H
#define VELK_EDITOR_CORE_PLUGIN_H

#include <velk/common.h>

namespace velk::editor {

namespace ClassId {
/** @brief The editor's state for one attached application. */
inline constexpr Uid EditorCore{"04f8be34-adf6-437e-b5db-814cfa286f78"};
} // namespace ClassId

namespace PluginId {
/** @brief The editor core (velk_editor_core). */
inline constexpr Uid CorePlugin{"34d7a491-0b35-4ed2-a78a-4b8043defaee"};
} // namespace PluginId

} // namespace velk::editor

#endif // VELK_EDITOR_CORE_PLUGIN_H
