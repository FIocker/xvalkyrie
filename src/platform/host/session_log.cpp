#include "platform/host/session_log.hpp"
#include <atomic>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// stdout and stderr are pointed at pipes, at the level of the OS handles so that a title,
// which has a C runtime of its own, is caught too. A helper process reads the pipes and
// copies what arrives to the log file and to where the stream went before. Being another
// process, it keeps copying until the pipes are closed, which is when this one is gone.
namespace xs::host {
namespace {
std::atomic<bool> active{false};
std::filesystem::path log_path(const std::filesystem::path &directory) {
    std::filesystem::create_directories(directory);
    const auto now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char name[32];
    std::strftime(name, sizeof name, "%Y-%m-%d_%H-%M-%S.log", &local);
    return directory / name;
}
} // namespace

#ifdef _WIN32
namespace {
HANDLE inheritable(HANDLE handle) {
    HANDLE copy = nullptr;
    if (!handle || handle == INVALID_HANDLE_VALUE ||
        !DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &copy, 0, TRUE,
                         DUPLICATE_SAME_ACCESS))
        return nullptr;
    return copy;
}
std::wstring number(HANDLE handle) {
    return std::to_wstring(reinterpret_cast<std::uintptr_t>(handle));
}
void point_at(DWORD which, std::FILE *stream, HANDLE write) {
    const HANDLE before = GetStdHandle(which);
    SetStdHandle(which, write);
    // without a console the C runtime has no such stream at all: give it one
    if (_fileno(stream) < 0)
        std::freopen("NUL", "w", stream);
    _dup2(_open_osfhandle(reinterpret_cast<intptr_t>(write), _O_BINARY), _fileno(stream));
    std::setvbuf(stream, nullptr, _IONBF, 0);
    // _dup2 closed the handle the stream had. Other C runtimes in the process (system
    // DLLs' and, later, the title's) still hold its value, and the next handle created
    // would take it over. Make that next handle this pipe, so they write here too.
    HANDLE stale[8];
    int count = 0;
    for (; count < 8; ++count) {
        if (!DuplicateHandle(GetCurrentProcess(), write, GetCurrentProcess(), &stale[count], 0,
                             FALSE, DUPLICATE_SAME_ACCESS))
            break;
        if (stale[count] == before) // kept open for good
            break;
    }
    for (int index = 0; index < count; ++index)
        CloseHandle(stale[index]);
}
void copy(HANDLE read, HANDLE file, HANDLE original) {
    char buffer[8192];
    DWORD count = 0, written = 0;
    while (ReadFile(read, buffer, sizeof buffer, &count, nullptr) && count) {
        WriteFile(file, buffer, count, &written, nullptr);
        if (original)
            WriteFile(original, buffer, count, &written, nullptr);
    }
}
} // namespace

std::filesystem::path start_session_log(const std::filesystem::path &directory) {
    if (active.exchange(true))
        throw std::logic_error("The session log is already running");
    const auto path = log_path(directory);
    std::fflush(nullptr);
    HANDLE read[2] = {}, write[2] = {};
    for (int stream = 0; stream < 2; ++stream) {
        // the write ends stay in this process only, or the helper would never see the end
        if (!CreatePipe(&read[stream], &write[stream], nullptr, 1 << 20) ||
            !SetHandleInformation(read[stream], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
            throw std::runtime_error("Cannot create the session log pipes");
    }
    const HANDLE originals[2] = {inheritable(GetStdHandle(STD_OUTPUT_HANDLE)),
                                 inheritable(GetStdHandle(STD_ERROR_HANDLE))};
    std::wstring executable(32768, L'\0');
    executable.resize(
        GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())));
    auto command = L"\"" + executable + L"\" " +
                   std::wstring(std::begin(helper_option), std::end(helper_option) - 1) + L" \"" +
                   path.wstring() + L"\"";
    for (const auto handle : {read[0], read[1], originals[0], originals[1]})
        command += L" " + number(handle);
    STARTUPINFOW startup{sizeof startup};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, 0, nullptr,
                        nullptr, &startup, &process)) {
        active = false;
        throw std::runtime_error("Cannot start the session log helper");
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    for (const auto handle : {read[0], read[1], originals[0], originals[1]})
        if (handle)
            CloseHandle(handle);
    point_at(STD_OUTPUT_HANDLE, stdout, write[0]);
    point_at(STD_ERROR_HANDLE, stderr, write[1]);
    return path;
}

int session_log_helper(const std::vector<std::wstring> &arguments) {
    if (arguments.size() != 5)
        return 2;
    const auto handle = [&](std::size_t index) {
        return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::stoull(arguments[index])));
    };
    const auto file =
        CreateFileW(arguments[0].c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return 1;
    std::thread errors(copy, handle(2), file, handle(4));
    copy(handle(1), file, handle(3));
    errors.join();
    return 0;
}
#else
std::filesystem::path start_session_log(const std::filesystem::path &directory) {
    if (active.exchange(true))
        throw std::logic_error("The session log is already running");
    const auto path = log_path(directory);
    const int file = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (file < 0) {
        active = false;
        throw std::runtime_error("Cannot create " + path.string());
    }
    std::fflush(nullptr);
    for (const int descriptor : {STDOUT_FILENO, STDERR_FILENO}) {
        int ends[2];
        if (pipe(ends) != 0)
            throw std::runtime_error("Cannot create the session log pipes");
        // one helper per stream; forked twice so that nobody has to wait for it
        const auto child = fork();
        if (child == 0) {
            if (fork() != 0)
                _exit(0);
            close(ends[1]);
            // only the read end, the file and this stream's old destination stay open here
            for (const int other : {STDOUT_FILENO, STDERR_FILENO})
                if (other != descriptor)
                    close(other);
            char buffer[8192];
            for (;;) {
                const auto count = read(ends[0], buffer, sizeof buffer);
                if (count <= 0)
                    break;
                if (write(file, buffer, static_cast<std::size_t>(count)) < 0 ||
                    write(descriptor, buffer, static_cast<std::size_t>(count)) < 0) {
                    // keep draining so that the writer never blocks
                }
            }
            _exit(0);
        }
        if (child > 0)
            waitpid(child, nullptr, 0);
        close(ends[0]);
        dup2(ends[1], descriptor);
        close(ends[1]);
    }
    close(file);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    return path;
}
int session_log_helper(const std::vector<std::wstring> &) { return 2; }
#endif
} // namespace xs::host
