#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include <windows.h>

// xgameruntime.dll, the system side of the Gaming Runtime (the GDK's XUser, XGameSave,
// XTaskQueue and the rest). Internal to the files of this folder.
namespace xs::runtime::modules::gameruntime {
struct Method {
    std::uint16_t slot;
    const char *name;
};
// One table QueryApiImpl hands out. `iid` is zero for the table every interface ID of the
// API gets, and names the one interface that has a table of its own otherwise.
struct Interface {
    const char *name;
    GUID api, iid;
    std::span<const Method> methods;
};
std::span<const Interface> known_interfaces();
// Puts `function` in every slot of every table where the method called `name` is. Its first
// argument is the table, the rest are the arguments of the title's function.
void implement(const char *name, void *function);
template <class Function> void implement(const char *name, Function *function) {
    implement(name, reinterpret_cast<void *>(function));
}

constexpr HRESULT e_pending = static_cast<HRESULT>(0x8000000A);
constexpr HRESULT e_not_sufficient_buffer = static_cast<HRESULT>(0x8007007A);

// XTaskQueue (task_queue.cpp)
struct Queue;
enum class Port : std::uint32_t { work, completion };
using TaskCallback = void CALLBACK(void *context, bool canceled);
// the process queue when `queue` is null; `delay` in milliseconds
HRESULT submit(Queue *queue, Port port, std::uint32_t delay, void *context,
               TaskCallback *callback);
Queue *duplicate(Queue *queue);
void close(Queue *queue);
// one more reference to the process queue, null if the title took it away
Queue *process_queue();

// XAsync (async.cpp)
struct XAsyncBlock {
    Queue *queue;
    void *context;
    void(CALLBACK *callback)(XAsyncBlock *block);
    void *internal[4];
};
// Starts and finishes an operation of ours on the title's block: it completes on the
// block's queue with `status`, and XAsyncGetResult under `identity` gives `payload`.
HRESULT finish(XAsyncBlock *block, const void *identity, HRESULT status,
               std::vector<std::uint8_t> payload = {});
// The result of an operation started with finish(): its status if it failed or has no
// payload, else the payload copied to `buffer`.
HRESULT result(XAsyncBlock *block, const void *identity, std::size_t size, void *buffer,
               std::size_t *used);
HRESULT result_size(XAsyncBlock *block, std::size_t *size);

// copies `text` and its terminator to a buffer of the title's, the Gaming Runtime's way
HRESULT copy_text(const std::string &text, std::size_t size, char *buffer, std::size_t *used);

void install_task_queue();
void install_async();
void install_user();
void install_save();
void install_system();
} // namespace xs::runtime::modules::gameruntime
