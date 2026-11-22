#ifndef VELK_EDITOR_PROCESS_H
#define VELK_EDITOR_PROCESS_H

#include <velk/string.h>
#include <velk/vector.h>

#include <cstdint>
#include <utility>

namespace velk::editor {

/** @brief A child process, ended when this object goes away. */
class ChildProcess
{
public:
    ChildProcess() = default;
    ~ChildProcess() { kill(); }
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    /**
     * @brief Starts @p exe with @p args and extra environment variables.
     *
     * The child's standard output goes to this process's standard error, so a stdio protocol
     * on standard output stays clean.
     */
    bool start(string_view exe, const vector<string>& args, const vector<std::pair<string, string>>& env);

    /** @brief True while the child runs. */
    bool running() const;

    /** @brief Ends the child, if it runs. */
    void kill();

private:
#ifdef _WIN32
    void* process_ = nullptr;
#else
    int pid_ = 0;
#endif
};

/** @brief A TCP port on localhost that nothing listens on right now; 0 on failure. */
uint16_t free_port();

/** @brief The folder of this process's executable, ending in a separator. */
string executable_folder();

} // namespace velk::editor

#endif // VELK_EDITOR_PROCESS_H
