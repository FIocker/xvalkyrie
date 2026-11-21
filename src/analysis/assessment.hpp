#pragma once
#include <nlohmann/json_fwd.hpp>
#include <string_view>

namespace xv::analysis {
nlohmann::json assess(const nlohmann::json &pe_report);
nlohmann::json compare_declaration(const nlohmann::json &pe_report,
                                   const nlohmann::json &config_report,
                                   std::string_view entry_name);
} // namespace xv::analysis
