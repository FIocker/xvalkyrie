#include "runtime/common/runtime.hpp"
#include "runtime/durango/runtime.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <windows.h>

#include <tlhelp32.h> // after windows.h

// XS_WATCHDOG: when no frame has been presented for 5 seconds, the stack of every thread of
// the process goes to the log, three times per stall, to see what the title is waiting on.
namespace xs::runtime::durango {
namespace {
std::atomic<std::int64_t> last_frame{0};
std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
void dump_thread(DWORD id) {
    auto *const thread = OpenThread(
        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
    if (!thread)
        return;
    if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
        CloseHandle(thread);
        return;
    }
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    std::string frames;
    if (GetThreadContext(thread, &context))
        for (int depth = 0; depth < 32 && context.Rip; ++depth) {
            frames += " " + detail::guest_location(reinterpret_cast<void *>(context.Rip));
            DWORD64 base = 0;
            auto *const function = RtlLookupFunctionEntry(context.Rip, &base, nullptr);
            if (!function) { // a leaf: the return address is on top of the stack
                context.Rip = *reinterpret_cast<DWORD64 *>(context.Rsp);
                context.Rsp += 8;
                continue;
            }
            void *handler_data = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, function, &context,
                             &handler_data, &frame, nullptr);
        }
    ResumeThread(thread);
    wchar_t *description = nullptr;
    std::string name;
    if (SUCCEEDED(GetThreadDescription(thread, &description)) && description) {
        for (const wchar_t *at = description; *at; ++at)
            name += *at < 0x80 ? static_cast<char>(*at) : '?';
        LocalFree(description);
    }
    CloseHandle(thread);
    std::fprintf(stderr, "[Runtime] thread %lu (%s):%s\n", id, name.c_str(), frames.c_str());
}
void dump_threads() {
    const auto self = GetCurrentThreadId();
    auto *const snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    THREADENTRY32 entry{sizeof entry};
    for (auto more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
        if (entry.th32OwnerProcessID == GetCurrentProcessId() && entry.th32ThreadID != self)
            dump_thread(entry.th32ThreadID);
    CloseHandle(snapshot);
    std::fflush(stderr);
}
} // namespace

void frame_watchdog() {
    static const bool enabled = std::getenv("XS_WATCHDOG") != nullptr;
    if (!enabled)
        return;
    if (last_frame.exchange(now()) == 0)
        std::thread([] {
            // three looks 5 seconds apart, to tell a thread that waits from one that spins
            int reported = 0;
            for (;;) {
                Sleep(1000);
                const auto stalled = now() - last_frame.load();
                if (stalled < 5000) {
                    reported = 0;
                } else if (reported < 3 && stalled > 5000ll * (reported + 1)) {
                    std::fprintf(stderr, "[Runtime] no frame for %lld seconds, the threads:\n",
                                 static_cast<long long>(stalled / 1000));
                    dump_threads();
                    ++reported;
                }
            }
        }).detach();
}
} // namespace xs::runtime::durango
