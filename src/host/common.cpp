#include "host/detail.hpp"
#include "host/threading.hpp"
#include <limits>

namespace xs::host {
Reservation::Reservation(std::size_t bytes) : page_(page_size()) {
    if (!bytes || bytes > std::numeric_limits<std::size_t>::max() - (page_ - 1))
        throw std::invalid_argument("Invalid host reservation size");
    size_ = ((bytes + page_ - 1) / page_) * page_;
    address_ = detail::reserve(size_);
}
Reservation::~Reservation() { detail::release(address_, size_); }
void Reservation::commit() {
    if (committed_)
        throw std::logic_error("Reservation is already committed");
    detail::commit(address_, size_);
    committed_ = true;
}
void Reservation::protect(std::size_t offset, std::size_t length, Access access) {
    if (!committed_)
        throw std::logic_error("Reservation is not committed");
    if (!length || offset % page_ || length % page_ || offset > size_ || length > size_ - offset)
        throw std::invalid_argument("Protection requires whole pages inside the reservation");
    detail::protect(address_ + offset, length, access);
}

void ManualEvent::signal() {
    {
        std::lock_guard lock(mutex_);
        signaled_ = true;
    }
    changed_.notify_all();
}
void ManualEvent::reset() {
    std::lock_guard lock(mutex_);
    signaled_ = false;
}
bool ManualEvent::wait(std::stop_token stop) {
    std::unique_lock lock(mutex_);
    return changed_.wait(lock, stop, [this] { return signaled_; });
}
} // namespace xs::host
