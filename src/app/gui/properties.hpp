#pragma once
#include "app/gui/theme.hpp"
#include <filesystem>
#include <future>
#include <nlohmann/json.hpp>
#include <string>

namespace xv::ui {
// A window with everything that can be told about a title without running it, for looking
// into why it does or does not work: what it is, where its image is mapped, the modules it
// imports and where each import binds. What it shows is title_properties() of the launcher,
// worked out off the window thread.
class Properties {
  public:
    void show(const std::filesystem::path &path, const std::string &name);
    void draw(const Fonts &fonts);
    // the title is still being read
    bool loading() const { return loading_.valid(); }

  private:
    void overview() const;
    void mapping() const;
    void libraries() const;
    void imports();

    bool open_ = false, raise_ = false;
    std::string name_;
    std::future<nlohmann::json> loading_;
    nlohmann::json report_;
    std::string raw_; // the report as text, for the last tab
    char filter_[64] = "";
    bool missing_only_ = false;
};
} // namespace xv::ui
