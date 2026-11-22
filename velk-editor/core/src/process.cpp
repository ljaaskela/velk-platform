#include "process.h"

#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace velk::editor {

uint16_t free_port()
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return 0;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        WSACleanup();
        return 0;
    }
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        return 0;
    }
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    uint16_t port = 0;
    socklen_t len = sizeof(addr);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0
        && getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
        port = ntohs(addr.sin_port);
    }
#ifdef _WIN32
    closesocket(s);
    WSACleanup();
#else
    close(s);
#endif
    return port;
}

#ifdef _WIN32

namespace {

std::wstring widen(string_view s)
{
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

void append_quoted(std::wstring& line, const std::wstring& arg)
{
    line += L'"';
    for (wchar_t c : arg) {
        if (c == L'"') {
            line += L'\\';
        }
        line += c;
    }
    if (!arg.empty() && arg.back() == L'\\') {
        line += L'\\';
    }
    line += L'"';
}

} // namespace

bool ChildProcess::start(string_view exe, const vector<string>& args, const vector<std::pair<string, string>>& env)
{
    kill();
    const std::wstring wexe = widen(exe);
    std::wstring line;
    append_quoted(line, wexe);
    for (auto& a : args) {
        line += L' ';
        append_quoted(line, widen(a));
    }

    // The child gets this process's environment plus env.
    std::wstring block;
    if (wchar_t* current = GetEnvironmentStringsW()) {
        for (const wchar_t* p = current; *p; p += wcslen(p) + 1) {
            bool replaced = false;
            for (auto& [name, value] : env) {
                const std::wstring prefix = widen(name) + L'=';
                if (_wcsnicmp(p, prefix.c_str(), prefix.size()) == 0) {
                    replaced = true;
                }
            }
            if (!replaced) {
                block += p;
                block += L'\0';
            }
        }
        FreeEnvironmentStringsW(current);
    }
    for (auto& [name, value] : env) {
        block += widen(name) + L'=' + widen(value);
        block += L'\0';
    }
    block += L'\0';

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nullptr;
    si.hStdOutput = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(wexe.c_str(), line.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT,
                        block.data(), nullptr, &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    return true;
}

bool ChildProcess::running() const
{
    return process_ && WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT;
}

void ChildProcess::kill()
{
    if (!process_) {
        return;
    }
    TerminateProcess(static_cast<HANDLE>(process_), 0);
    WaitForSingleObject(static_cast<HANDLE>(process_), 5000);
    CloseHandle(static_cast<HANDLE>(process_));
    process_ = nullptr;
}

string executable_folder()
{
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf, n);
    path = path.substr(0, path.find_last_of(L"\\/") + 1);
    const int size = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), out.data(), size, nullptr,
                        nullptr);
    return string(string_view(out.data(), out.size()));
}

#else

bool ChildProcess::start(string_view exe, const vector<string>& args, const vector<std::pair<string, string>>& env)
{
    kill();
    vector<string> storage;
    storage.push_back(string(exe));
    for (auto& a : args) {
        storage.push_back(a);
    }
    vector<char*> argv;
    for (auto& s : storage) {
        argv.push_back(const_cast<char*>(s.c_str()));
    }
    argv.push_back(nullptr);

    vector<string> env_storage;
    for (char** e = environ; *e; ++e) {
        bool replaced = false;
        for (auto& [name, value] : env) {
            if (std::strncmp(*e, name.c_str(), name.size()) == 0 && (*e)[name.size()] == '=') {
                replaced = true;
            }
        }
        if (!replaced) {
            env_storage.push_back(string(string_view(*e, std::strlen(*e))));
        }
    }
    for (auto& [name, value] : env) {
        env_storage.push_back(name + "=" + value);
    }
    vector<char*> envp;
    for (auto& s : env_storage) {
        envp.push_back(const_cast<char*>(s.c_str()));
    }
    envp.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, STDERR_FILENO, STDOUT_FILENO);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, storage[0].c_str(), &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) {
        return false;
    }
    pid_ = pid;
    return true;
}

bool ChildProcess::running() const
{
    if (!pid_) {
        return false;
    }
    int status = 0;
    return waitpid(pid_, &status, WNOHANG) == 0;
}

void ChildProcess::kill()
{
    if (!pid_) {
        return;
    }
    ::kill(pid_, SIGTERM);
    int status = 0;
    waitpid(pid_, &status, 0);
    pid_ = 0;
}

string executable_folder()
{
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        return string("./");
    }
    std::string path(buf, static_cast<size_t>(n));
    path = path.substr(0, path.find_last_of('/') + 1);
    return string(string_view(path.data(), path.size()));
}

#endif

} // namespace velk::editor
