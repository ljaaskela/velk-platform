#ifndef VELK_RUNTIME_INTF_RUNTIME_PLUGIN_H
#define VELK_RUNTIME_INTF_RUNTIME_PLUGIN_H

#include <velk/interface/intf_interface.h>
#include <velk/string.h>

#include <velk-runtime/interface/intf_application.h>

namespace velk {

/**
 * @brief The contents of an application's app.velk.
 *
 * Paths are absolute, with forward slashes. Relative paths in the file are taken from the
 * folder app.velk is in.
 */
struct AppConfigFile
{
    string folder;         ///< Folder app.velk is in, ending in '/'.
    ApplicationConfig app; ///< For create_app(). app_root is the folder unless the file sets it.
    WindowConfig window;   ///< For create_window().
    string store;          ///< The first store file.
    string error;          ///< Why reading failed, or empty.
};

/**
 * @brief Interface of the velk-runtime plugin.
 *
 * Chain: IInterface -> IRuntimePlugin
 */
class IRuntimePlugin : public Interface<IRuntimePlugin>
{
public:
    /**
     * @brief Reads an app.velk file.
     * @param path The file's path.
     * @param out  Receives the contents, or the reason in @c out.error.
     * @return @c true on success.
     */
    virtual bool read_app_config(string_view path, AppConfigFile& out) const = 0;
};

} // namespace velk

#endif // VELK_RUNTIME_INTF_RUNTIME_PLUGIN_H
