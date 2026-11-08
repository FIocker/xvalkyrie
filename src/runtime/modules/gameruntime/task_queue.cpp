#include "runtime/modules/gameruntime/gameruntime.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

// XTaskQueue: where a title's asynchronous work and its completion callbacks run. A queue
// has two ports, work and completion, each with its own way of running callbacks: on the
// thread pool, on the thread pool one at a time, inline, or only when the title asks
// (XTaskQueueDispatch, usually once a frame).
namespace xs::runtime::modules::gameruntime {
namespace {
enum class Mode : std::uint32_t { manual, thread_pool, serialized, immediate };
struct Entry {
    Queue *queue;
    void *context;
    TaskCallback *callback;
};
struct PortState : std::enable_shared_from_this<PortState> {
    explicit PortState(Mode mode) : mode(mode) {}
    const Mode mode;
    std::mutex mutex;
    std::condition_variable available;
    std::deque<Entry> entries;
    bool draining = false; // serialized: a thread pool callback is going through the entries
};
struct Monitor {
    void *context;
    void(CALLBACK *callback)(void *context, Queue *queue, Port port);
};
struct Waiter {
    Queue *queue;
    Port port;
    void *context;
    TaskCallback *callback;
    HANDLE handle;
    PTP_WAIT wait;
};
} // namespace

struct Queue {
    std::atomic<long> references{1};
    std::shared_ptr<PortState> ports[2];
    std::atomic<bool> terminated{false};
    std::mutex mutex;
    std::map<std::uint64_t, Monitor> monitors;
    std::map<std::uint64_t, std::unique_ptr<Waiter>> waiters;
    std::uint64_t next_token = 1;
};

namespace {
std::mutex process_mutex;
Queue *process = nullptr;
bool process_made = false;

Queue *make_queue(std::shared_ptr<PortState> work, std::shared_ptr<PortState> completion) {
    auto *const queue = new Queue;
    queue->ports[0] = std::move(work);
    queue->ports[1] = std::move(completion);
    return queue;
}
bool take(PortState &port, Entry &entry) {
    const std::lock_guard lock(port.mutex);
    if (port.entries.empty())
        return false;
    entry = port.entries.front();
    port.entries.pop_front();
    return true;
}
void run(const Entry &entry) {
    entry.callback(entry.context, entry.queue->terminated);
    close(entry.queue);
}
void CALLBACK run_one(PTP_CALLBACK_INSTANCE, void *context) {
    if (Entry entry; take(*static_cast<PortState *>(context), entry))
        run(entry);
}
void CALLBACK run_all(PTP_CALLBACK_INSTANCE, void *context) {
    auto &port = *static_cast<PortState *>(context);
    for (;;) {
        Entry entry;
        {
            const std::lock_guard lock(port.mutex);
            if (port.entries.empty()) {
                port.draining = false;
                return;
            }
            entry = port.entries.front();
            port.entries.pop_front();
        }
        run(entry);
    }
}
HRESULT submit_now(Queue *queue, Port which, void *context, TaskCallback *callback) {
    if (!callback)
        return E_INVALIDARG;
    if (queue->terminated)
        return E_ABORT;
    auto &port = *queue->ports[static_cast<std::size_t>(which) & 1];
    if (port.mode == Mode::immediate) {
        callback(context, false);
        return S_OK;
    }
    ++queue->references;
    bool start = false;
    {
        const std::lock_guard lock(port.mutex);
        port.entries.push_back({queue, context, callback});
        if (port.mode == Mode::serialized && !port.draining)
            start = port.draining = true;
    }
    if (port.mode == Mode::manual)
        port.available.notify_one();
    else if (port.mode == Mode::thread_pool)
        TrySubmitThreadpoolCallback(&run_one, &port, nullptr);
    else if (start)
        TrySubmitThreadpoolCallback(&run_all, &port, nullptr);
    std::vector<Monitor> monitors;
    {
        const std::lock_guard lock(queue->mutex);
        for (const auto &[token, monitor] : queue->monitors)
            monitors.push_back(monitor);
    }
    for (const auto &monitor : monitors)
        monitor.callback(monitor.context, queue, which);
    return S_OK;
}
struct Delayed {
    Queue *queue;
    Port port;
    void *context;
    TaskCallback *callback;
};
void CALLBACK delay_elapsed(PTP_CALLBACK_INSTANCE, void *context, PTP_TIMER timer) {
    const std::unique_ptr<Delayed> delayed(static_cast<Delayed *>(context));
    if (FAILED(submit_now(delayed->queue, delayed->port, delayed->context, delayed->callback)))
        delayed->callback(delayed->context, true);
    close(delayed->queue);
    CloseThreadpoolTimer(timer);
}
void CALLBACK waiter_signaled(PTP_CALLBACK_INSTANCE, void *context, PTP_WAIT wait, TP_WAIT_RESULT) {
    auto *const waiter = static_cast<Waiter *>(context);
    submit_now(waiter->queue, waiter->port, waiter->context, waiter->callback);
    SetThreadpoolWait(wait, waiter->handle, nullptr);
}

HRESULT WINAPI create(void *, Mode work, Mode completion, Queue **queue) {
    if (!queue)
        return E_POINTER;
    *queue = make_queue(std::make_shared<PortState>(work), std::make_shared<PortState>(completion));
    return S_OK;
}
HRESULT WINAPI create_composite(void *, PortState *work, PortState *completion, Queue **queue) {
    if (!queue || !work || !completion)
        return E_POINTER;
    *queue = make_queue(work->shared_from_this(), completion->shared_from_this());
    return S_OK;
}
HRESULT WINAPI get_port(void *, Queue *queue, Port port, PortState **handle) {
    if (!queue || !handle)
        return E_POINTER;
    *handle = queue->ports[static_cast<std::size_t>(port) & 1].get();
    return S_OK;
}
HRESULT WINAPI duplicate_handle(void *, Queue *queue, Queue **duplicated) {
    if (!queue || !duplicated)
        return E_POINTER;
    *duplicated = duplicate(queue);
    return S_OK;
}
bool WINAPI dispatch(void *, Queue *queue, Port which, std::uint32_t timeout) {
    if (!queue)
        return false;
    auto &port = *queue->ports[static_cast<std::size_t>(which) & 1];
    if (port.mode != Mode::manual)
        return false;
    Entry entry;
    {
        std::unique_lock lock(port.mutex);
        const auto ready = [&] { return !port.entries.empty(); };
        if (timeout == INFINITE)
            port.available.wait(lock, ready);
        else if (!port.available.wait_for(lock, std::chrono::milliseconds(timeout), ready))
            return false;
        entry = port.entries.front();
        port.entries.pop_front();
    }
    run(entry);
    return true;
}
void WINAPI close_handle(void *, Queue *queue) {
    if (queue)
        close(queue);
}
HRESULT WINAPI submit_callback(void *, Queue *queue, Port port, void *context,
                               TaskCallback *callback) {
    return submit(queue, port, 0, context, callback);
}
HRESULT WINAPI submit_delayed(void *, Queue *queue, Port port, std::uint32_t delay, void *context,
                              TaskCallback *callback) {
    return submit(queue, port, delay, context, callback);
}
HRESULT WINAPI register_waiter(void *, Queue *queue, Port port, HANDLE handle, void *context,
                               TaskCallback *callback, std::uint64_t *token) {
    if (!queue || !callback || !token)
        return E_INVALIDARG;
    auto waiter = std::make_unique<Waiter>(Waiter{queue, port, context, callback, handle, nullptr});
    waiter->wait = CreateThreadpoolWait(&waiter_signaled, waiter.get(), nullptr);
    if (!waiter->wait)
        return HRESULT_FROM_WIN32(GetLastError());
    SetThreadpoolWait(waiter->wait, handle, nullptr);
    const std::lock_guard lock(queue->mutex);
    *token = queue->next_token++;
    queue->waiters[*token] = std::move(waiter);
    return S_OK;
}
void WINAPI unregister_waiter(void *, Queue *queue, std::uint64_t token) {
    if (!queue)
        return;
    std::unique_ptr<Waiter> waiter;
    {
        const std::lock_guard lock(queue->mutex);
        const auto found = queue->waiters.find(token);
        if (found == queue->waiters.end())
            return;
        waiter = std::move(found->second);
        queue->waiters.erase(found);
    }
    SetThreadpoolWait(waiter->wait, nullptr, nullptr);
    WaitForThreadpoolWaitCallbacks(waiter->wait, TRUE);
    CloseThreadpoolWait(waiter->wait);
}
// What was waiting in the queue is called as canceled, then the title's callback
HRESULT WINAPI terminate(void *, Queue *queue, bool, void *context,
                         void(CALLBACK *callback)(void *context)) {
    if (!queue)
        return E_INVALIDARG;
    queue->terminated = true;
    for (auto &port : queue->ports) {
        std::deque<Entry> mine;
        {
            const std::lock_guard lock(port->mutex);
            for (auto entry = port->entries.begin(); entry != port->entries.end();)
                if (entry->queue == queue) {
                    mine.push_back(*entry);
                    entry = port->entries.erase(entry);
                } else
                    ++entry;
        }
        for (const auto &entry : mine)
            run(entry);
    }
    if (callback)
        callback(context);
    return S_OK;
}
HRESULT WINAPI register_monitor(void *, Queue *queue, void *context,
                                void(CALLBACK *callback)(void *, Queue *, Port),
                                std::uint64_t *token) {
    if (!queue || !callback || !token)
        return E_INVALIDARG;
    const std::lock_guard lock(queue->mutex);
    *token = queue->next_token++;
    queue->monitors[*token] = {context, callback};
    return S_OK;
}
void WINAPI unregister_monitor(void *, Queue *queue, std::uint64_t token) {
    if (!queue)
        return;
    const std::lock_guard lock(queue->mutex);
    queue->monitors.erase(token);
}
bool WINAPI get_process_queue(void *, Queue **queue) {
    if (!queue)
        return false;
    *queue = process_queue();
    return *queue != nullptr;
}
void WINAPI set_process_queue(void *, Queue *queue) {
    Queue *old;
    {
        const std::lock_guard lock(process_mutex);
        old = process;
        process = queue ? duplicate(queue) : nullptr;
        process_made = true;
    }
    if (old)
        close(old);
}
// XThread: a thread the title says must not block has nothing to tell the host
thread_local bool time_sensitive = false;
HRESULT WINAPI set_time_sensitive(void *, bool sensitive) {
    time_sensitive = sensitive;
    return S_OK;
}
bool WINAPI is_time_sensitive(void *) { return time_sensitive; }
void WINAPI assert_not_time_sensitive(void *) {}
} // namespace

HRESULT submit(Queue *queue, Port port, std::uint32_t delay, void *context,
               TaskCallback *callback) {
    Queue *owned = nullptr;
    if (!queue)
        queue = owned = process_queue();
    if (!queue)
        return E_FAIL;
    auto result = S_OK;
    if (!delay) {
        result = submit_now(queue, port, context, callback);
    } else if (!callback) {
        result = E_INVALIDARG;
    } else {
        auto *const delayed = new Delayed{duplicate(queue), port, context, callback};
        if (auto *const timer = CreateThreadpoolTimer(&delay_elapsed, delayed, nullptr)) {
            ULARGE_INTEGER due;
            due.QuadPart = static_cast<ULONGLONG>(-(static_cast<LONGLONG>(delay) * 10000));
            FILETIME time{due.LowPart, due.HighPart};
            SetThreadpoolTimer(timer, &time, 0, 0);
        } else {
            close(delayed->queue);
            delete delayed;
            result = E_OUTOFMEMORY;
        }
    }
    if (owned)
        close(owned);
    return result;
}
Queue *duplicate(Queue *queue) {
    ++queue->references;
    return queue;
}
void close(Queue *queue) {
    if (--queue->references == 0)
        delete queue;
}
// the default one runs both ports on the thread pool, until the title sets its own
Queue *process_queue() {
    const std::lock_guard lock(process_mutex);
    if (!process_made) {
        process = make_queue(std::make_shared<PortState>(Mode::thread_pool),
                             std::make_shared<PortState>(Mode::thread_pool));
        process_made = true;
    }
    return process ? duplicate(process) : nullptr;
}

void install_task_queue() {
    implement("XTaskQueueCreate", &create);
    implement("XTaskQueueCreateComposite", &create_composite);
    implement("XTaskQueueGetPort", &get_port);
    implement("XTaskQueueDuplicateHandle", &duplicate_handle);
    implement("XTaskQueueDispatch", &dispatch);
    implement("XTaskQueueCloseHandle", &close_handle);
    implement("XTaskQueueSubmitCallback", &submit_callback);
    implement("XTaskQueueSubmitDelayedCallback", &submit_delayed);
    implement("XTaskQueueRegisterWaiter", &register_waiter);
    implement("XTaskQueueUnregisterWaiter", &unregister_waiter);
    implement("XTaskQueueTerminate", &terminate);
    implement("XTaskQueueRegisterMonitor", &register_monitor);
    implement("XTaskQueueUnregisterMonitor", &unregister_monitor);
    implement("XTaskQueueGetCurrentProcessTaskQueue", &get_process_queue);
    implement("XTaskQueueSetCurrentProcessTaskQueue", &set_process_queue);
    implement("XThreadSetTimeSensitive", &set_time_sensitive);
    implement("XThreadIsTimeSensitive", &is_time_sensitive);
    implement("XThreadAssertNotTimeSensitive", &assert_not_time_sensitive);
    implement("XThreadVerifyNotTimeSensitive", &assert_not_time_sensitive);
}
} // namespace xs::runtime::modules::gameruntime
