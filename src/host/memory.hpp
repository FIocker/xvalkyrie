#pragma once
#include <cstddef>

namespace xv::host {
// read_execute is only used by the runtime
enum class Access { none, read, read_write, read_execute };
std::size_t page_size();

class Reservation {
  public:
    explicit Reservation(std::size_t bytes);
    ~Reservation();
    Reservation(const Reservation &) = delete;
    Reservation &operator=(const Reservation &) = delete;

    // Commit the whole reservation once, then change access on individual pages.
    void commit();
    void protect(std::size_t offset, std::size_t length, Access access);
    std::byte *address() const { return address_; }
    std::size_t size() const { return size_; }

  private:
    std::size_t page_, size_;
    std::byte *address_;
    bool committed_ = false;
};
} // namespace xv::host
