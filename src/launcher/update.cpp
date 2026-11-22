#include "launcher/update.hpp"
#include "host/files.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <vector>
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>

namespace xv {
namespace {
constexpr wchar_t releases_host[] = L"api.github.com";
constexpr wchar_t latest_release[] = L"/repos/sammwyy/XScarlet/releases/latest";
constexpr char archive_name[] = "XValkyrie-Win-x86_64-Portable.zip";
constexpr std::array<const wchar_t *, 2> executables{L"xvalkyrie.exe", L"xvboot.exe"};
constexpr wchar_t replaced[] = L".replaced";

struct NotFound {};
struct Handle {
    HINTERNET handle = nullptr;
    ~Handle() {
        if (handle)
            WinHttpCloseHandle(handle);
    }
    explicit operator bool() const { return handle != nullptr; }
};
std::wstring wide(std::string_view text) {
    std::wstring out(MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0),
                     L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                        static_cast<int>(out.size()));
    return out;
}
std::vector<std::uint8_t> download(const std::wstring &address, std::size_t limit) {
    URL_COMPONENTS parts{sizeof parts};
    wchar_t host[256], path[2048];
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    if (!WinHttpCrackUrl(address.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
        throw std::runtime_error("the update's address is not an HTTPS one");
    const Handle session{WinHttpOpen(L"XValkyrie", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    const Handle connection{session ? WinHttpConnect(session.handle, host, parts.nPort, 0)
                                    : nullptr};
    const Handle request{connection
                             ? WinHttpOpenRequest(connection.handle, L"GET", path, nullptr,
                                                  WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                  WINHTTP_FLAG_SECURE)
                             : nullptr};
    if (!request ||
        !WinHttpSendRequest(request.handle, L"Accept: application/vnd.github+json, */*\r\n",
                            static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.handle, nullptr))
        throw std::runtime_error("GitHub could not be reached");
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status == 404)
        throw NotFound{};
    if (status != 200)
        throw std::runtime_error("GitHub answered " + std::to_string(status));
    std::vector<std::uint8_t> body;
    for (;;) {
        DWORD available = 0, read = 0;
        if (!WinHttpQueryDataAvailable(request.handle, &available))
            throw std::runtime_error("the download broke off");
        if (!available)
            return body;
        if (body.size() + available > limit)
            throw std::runtime_error("the download is larger than an update is");
        const auto at = body.size();
        body.resize(at + available);
        if (!WinHttpReadData(request.handle, body.data() + at, available, &read))
            throw std::runtime_error("the download broke off");
        body.resize(at + read);
    }
}
std::array<int, 3> numbers(std::string_view tag, bool &more) {
    std::array<int, 3> out{};
    std::size_t at = !tag.empty() && tag[0] == 'v' ? 1 : 0;
    for (auto &number : out) {
        while (at < tag.size() && tag[at] >= '0' && tag[at] <= '9')
            number = number * 10 + (tag[at++] - '0');
        if (at < tag.size() && tag[at] == '.')
            ++at;
    }
    more = at < tag.size();
    return out;
}
DWORD run(const std::filesystem::path &program, const std::wstring &arguments, bool elevated) {
    SHELLEXECUTEINFOW info{sizeof info};
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_NO_CONSOLE;
    info.lpVerb = elevated ? L"runas" : L"open";
    info.lpFile = program.c_str();
    info.lpParameters = arguments.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || !info.hProcess)
        throw std::runtime_error(elevated ? "the update was not allowed to change the program's folder"
                                          : "a program the update needs did not start");
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    return code;
}
bool writable(const std::filesystem::path &folder) {
    const auto probe = folder / L"xvalkyrie.update";
    const auto file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(file);
    return true;
}
} // namespace

UpdateCheck find_update(std::string_view current) try {
    const auto body = download(std::wstring(L"https://") + releases_host + latest_release, 4 << 20);
    const auto release = nlohmann::json::parse(body.begin(), body.end());
    Update update;
    update.version = release.at("tag_name").get<std::string>();
    for (const auto &asset : release.at("assets"))
        if (asset.at("name").get<std::string>() == archive_name)
            update.archive = asset.at("browser_download_url").get<std::string>();
    bool between = false, pre = false;
    const auto have = numbers(current, between), latest = numbers(update.version, pre);
    if (pre || latest < have || (latest == have && !between))
        return {std::nullopt, "XValkyrie is up to date"};
    if (update.archive.empty())
        return {std::nullopt, update.version + " is out, without a download for Windows yet"};
    return {update, {}};
} catch (const NotFound &) {
    return {std::nullopt, "No release has been published yet"};
} catch (const std::exception &e) {
    return {std::nullopt, std::string("Could not check for updates: ") + e.what()};
} catch (...) {
    return {std::nullopt, "Could not check for updates"};
}

void apply_update(const std::filesystem::path &staged) {
    const auto folder = host::executable_directory();
    for (const auto *name : executables)
        if (!std::filesystem::exists(staged / name))
            throw std::runtime_error("the update's archive lacks an executable");
    std::vector<const wchar_t *> moved;
    try {
        for (const auto *name : executables) {
            const auto old = folder / (std::wstring(name) + replaced);
            std::error_code ignored;
            std::filesystem::remove(old, ignored);
            if (std::filesystem::exists(folder / name))
                std::filesystem::rename(folder / name, old);
            moved.push_back(name);
            std::filesystem::copy_file(staged / name, folder / name);
        }
    } catch (const std::exception &) {
        for (const auto *name : moved) {
            std::error_code ignored;
            std::filesystem::remove(folder / name, ignored);
            std::filesystem::rename(folder / (std::wstring(name) + replaced), folder / name, ignored);
        }
        throw std::runtime_error("the program's files could not be replaced");
    }
}

void install_update(const Update &update) try {
    const auto archive_bytes = download(wide(update.archive), 512u << 20);
    wchar_t temporary[MAX_PATH];
    GetTempPathW(MAX_PATH, temporary);
    const auto staged = std::filesystem::path(temporary) /
                        (L"xvalkyrie-update-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ignored;
    std::filesystem::remove_all(staged, ignored);
    std::filesystem::create_directories(staged);
    const auto archive = staged / L"update.zip";
    {
        std::ofstream file(archive, std::ios::binary);
        file.write(reinterpret_cast<const char *>(archive_bytes.data()),
                   static_cast<std::streamsize>(archive_bytes.size()));
        if (!file)
            throw std::runtime_error("the update could not be saved");
    }
    // tar.exe of System32 reads zip
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    if (run(std::filesystem::path(system) / L"tar.exe",
            L"-xf \"" + archive.wstring() + L"\" -C \"" + staged.wstring() + L"\"", false) != 0)
        throw std::runtime_error("the update's archive could not be opened");
    const auto folder = host::executable_directory();
    if (writable(folder)) {
        apply_update(staged);
    } else if (run(folder / executables[0], L"--apply-update \"" + staged.wstring() + L"\"", true) != 0) {
        throw std::runtime_error("the program's files could not be replaced");
    }
    std::filesystem::remove_all(staged, ignored);
} catch (const NotFound &) {
    throw std::runtime_error("the release's download is not there");
} catch (const std::filesystem::filesystem_error &) {
    throw std::runtime_error("the update's files could not be written");
}

void restart() {
    const auto program = host::executable_directory() / executables[0];
    ShellExecuteW(nullptr, L"open", program.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void clear_replaced() {
    for (const auto *name : executables) {
        std::error_code ignored;
        std::filesystem::remove(host::executable_directory() / (std::wstring(name) + replaced), ignored);
    }
}
} // namespace xv
