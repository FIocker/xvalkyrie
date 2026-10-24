#include "formats/appx/manifest.hpp"
#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include <stdexcept>
#include <string_view>

namespace xs::appx {
namespace {
using json = nlohmann::json;
json attribute(pugi::xml_node node, const char *name) {
    const auto attr = node.attribute(name);
    return attr ? json(attr.value()) : json(nullptr);
}
json text(pugi::xml_node node) { return node ? json(node.text().get()) : json(nullptr); }
} // namespace

nlohmann::json inspect_manifest(std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > manifest_limit)
        throw std::runtime_error("Expected a package manifest of 1 byte to 1 MiB");
    const std::string_view xml(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    if (xml.find("<!DOCTYPE") != std::string_view::npos)
        throw std::runtime_error("DTD declarations are not supported in package manifests");
    pugi::xml_document doc;
    const auto result = doc.load_buffer(bytes.data(), bytes.size());
    if (!result)
        throw std::runtime_error(std::string("Package manifest XML: ") + result.description());
    const auto root = doc.document_element();
    if (std::string_view(root.name()) != "Package")
        throw std::runtime_error("Expected Package root element");
    const auto identity = root.child("Identity");
    if (!identity)
        throw std::runtime_error("Package manifest has no Identity");
    auto applications = json::array();
    for (auto entry : root.child("Applications").children("Application")) {
        if (applications.size() >= 64)
            throw std::runtime_error("Package manifest exceeds 64 applications");
        if (!*entry.attribute("Executable").value())
            throw std::runtime_error("Package manifest application has no Executable");
        applications.push_back({{"id", attribute(entry, "Id")},
                                {"executable", entry.attribute("Executable").value()},
                                {"entry_point", attribute(entry, "EntryPoint")}});
    }
    if (applications.empty())
        throw std::runtime_error("Package manifest has no applications");
    const auto prerequisites = root.child("Prerequisites");
    auto logo = text(root.child("Properties").child("Logo"));
    if (logo.is_null()) {
        for (auto application : root.child("Applications").children("Application")) {
            for (auto child : application.children()) {
                const auto square = child.attribute("Square150x150Logo");
                const auto small = child.attribute("Square44x44Logo");
                if (square || small) {
                    logo = square ? json(square.value()) : json(small.value());
                    break;
                }
            }
            if (!logo.is_null())
                break;
        }
    }
    // <SplashScreen Image="..."/> inside the visual elements of an application, whatever
    // namespace prefix the manifest version gives either of them
    auto splash = json(nullptr);
    for (auto application : root.child("Applications").children("Application"))
        for (auto visual : application.children())
            for (auto child : visual.children())
                if (splash.is_null() && std::string_view(child.name()).ends_with("SplashScreen") &&
                    child.attribute("Image"))
                    splash = child.attribute("Image").value();
    // <mx:XboxLive TitleId="..." PrimaryServiceConfigId="..."/>, wherever it is
    const auto live = root.find_node([](pugi::xml_node node) {
        return std::string_view(node.name()).ends_with("XboxLive") && node.attribute("TitleId");
    });
    // WinRT classes implemented by DLLs of the package: class name -> DLL path as written
    auto classes = json::object();
    for (auto extension : root.child("Extensions").children()) {
        if (std::string_view(extension.attribute("Category").value()) !=
            "windows.activatableClass.inProcessServer")
            continue;
        const auto server = extension.child("InProcessServer");
        const std::string_view path = server.child("Path").text().get();
        if (path.empty())
            throw std::runtime_error("Package manifest in-process server has no Path");
        for (auto activatable : server.children("ActivatableClass")) {
            if (classes.size() >= 4096)
                throw std::runtime_error("Package manifest exceeds 4096 activatable classes");
            const std::string_view id = activatable.attribute("ActivatableClassId").value();
            if (id.empty())
                throw std::runtime_error("Package manifest activatable class has no id");
            classes[std::string(id)] = path;
        }
    }
    return {{"name", attribute(identity, "Name")},
            {"version", attribute(identity, "Version")},
            {"architecture", attribute(identity, "ProcessorArchitecture")},
            {"display_name", text(root.child("Properties").child("DisplayName"))},
            {"logo", std::move(logo)},
            {"splash", std::move(splash)},
            {"os_name", text(prerequisites.child("mx:OSName"))},
            {"environment", text(prerequisites.child("mx:ApplicationEnvironment"))},
            {"applications", std::move(applications)},
            {"xbox_live_title_id", attribute(live, "TitleId")},
            {"service_config_id", attribute(live, "PrimaryServiceConfigId")},
            {"activatable_classes", std::move(classes)}};
}
} // namespace xs::appx
