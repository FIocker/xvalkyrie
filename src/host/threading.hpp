#pragma once
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stop_token>

namespace xs::host {
// Slot values are non-owning. Join participating threads before destroying a slot.
class ThreadSlot {
  public:
    ThreadSlot();
    ~ThreadSlot();
    ThreadSlot(const ThreadSlot &) = delete;
    ThreadSlot &operator=(const ThreadSlot &) = delete;
    void set(void *value);
    void *get() const;

  private:
    std::uint32_t key_;
};

class ManualEvent {
  public:
    void signal();
    void reset();
    // True if signaled, false if canceled while the event was clear.
    bool wait(std::stop_token stop = {});

  private:
    std::mutex mutex_;
    std::condition_variable_any changed_;
    bool signaled_ = false;
};
} // namespace xs::host
