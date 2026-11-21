#pragma once
#include <filesystem>

namespace xv::host {
// Loading runs host module initializers. Only pass trusted host-owned paths.
class NativeModule {
  public:
    using Function = void (*)();
    explicit NativeModule(const std::filesystem::path &trusted_path);
    ~NativeModule();
    NativeModule(const NativeModule &) = delete;
    NativeModule &operator=(const NativeModule &) = delete;
    Function function(const char *name) const;

  private:
    void *handle_;
};
} // namespace xv::host
