#include "formats/gdk/game_config.hpp"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include <set>
#include <stdexcept>
#include <string_view>

namespace xv::gdk {
namespace {
using json = nlohmann::json;
json attribute(pugi::xml_node node, const char *name) {
    const auto attr = node.attribute(name);
    return attr ? json(attr.value()) : json(nullptr);
}
void check_element(pugi::xml_node node) {
    std::set<std::string_view> names;
    for (auto attr : node.attributes())
        if (!names.insert(attr.name()).second)
            throw std::runtime_error("Duplicate game config attribute");
    if (node.attribute("xmlns") && std::string_view(node.attribute("xmlns").value()) != "")
        throw std::runtime_error("Unsupported game config XML namespace");
}
} // namespace

nlohmann::json inspect_config(std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > config_limit)
        throw std::runtime_error("Expected a game config of 1 byte to 1 MiB");
    if (std::find(bytes.begin(), bytes.end(), 0) != bytes.end())
        throw std::runtime_error("Only UTF-8 game configs without embedded NUL are supported");
    // Do not let the XML parser process a DTD, including an internal subset.
    const std::string_view xml(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    if (xml.find("<!DOCTYPE") != std::string_view::npos)
        throw std::runtime_error("DTD declarations are not supported in game configs");
    pugi::xml_document doc;
    const auto result = doc.load_buffer(
        bytes.data(), bytes.size(), pugi::parse_default | pugi::parse_doctype, pugi::encoding_utf8);
    if (!result)
        throw std::runtime_error(std::string("Game config XML: ") + result.description());
    const auto root = doc.document_element();
    for (auto node : doc.children())
        if (node.type() == pugi::node_doctype ||
            (node.type() == pugi::node_element && node != root))
            throw std::runtime_error("Expected one game config root without a DTD");
    if (std::string_view(root.name()) != "Game")
        throw std::runtime_error("Expected Game root element");
    check_element(root);
    const auto list = root.child("ExecutableList");
    if (!list || list.next_sibling("ExecutableList"))
        throw std::runtime_error("Expected one Game/ExecutableList");
    check_element(list);
    auto entries = json::array();
    for (auto entry : list.children("Executable")) {
        check_element(entry);
        if (entries.size() >= 256)
            throw std::runtime_error("Game config exceeds 256 executable entries");
        if (!*entry.attribute("Name").value())
            throw std::runtime_error("Game config executable has no Name");
        const auto family = attribute(entry, "TargetDeviceFamily");
        entries.push_back(
            {{"name", entry.attribute("Name").value()},
             {"declared_device_family", family},
             {"device_family_status",
              family.is_null()                                                ? "unspecified"
              : family == "Scarlett" || family == "XboxOne" || family == "PC" ? "documented_value"
                                                                              : "unknown_value"},
             {"declared_architecture", attribute(entry, "Architecture")},
             {"id", attribute(entry, "Id")}});
    }
    if (entries.empty())
        throw std::runtime_error("Game config has no executable entries");
    const auto identity = root.child("Identity");
    const auto visuals = root.child("ShellVisuals");
    const auto text = [&](const char *name) {
        const auto node = root.child(name);
        return node ? json(node.text().get()) : json(nullptr);
    };
    return {{"evidence_kind", "config_declaration"},
            {"config_version", attribute(root, "configVersion")},
            {"name", attribute(identity, "Name")},
            {"publisher", attribute(identity, "Publisher")},
            {"version", attribute(identity, "Version")},
            {"display_name", attribute(visuals, "DefaultDisplayName")},
            {"publisher_display_name", attribute(visuals, "PublisherDisplayName")},
            {"logo", attribute(visuals, "Square150x150Logo")},
            {"store_logo", attribute(visuals, "StoreLogo")},
            {"large_logo", attribute(visuals, "Square480x480Logo")},
            {"small_logo", attribute(visuals, "Square44x44Logo")},
            {"splash_screen", attribute(visuals, "SplashScreenImage")},
            {"title_id", text("TitleId")},
            {"store_id", text("StoreId")},
            {"executables", std::move(entries)},
            {"console_target_verified", false}};
}
} // namespace xv::gdk
