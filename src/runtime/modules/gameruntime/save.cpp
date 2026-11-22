#include "runtime/common/runtime.hpp"
#include "runtime/modules/gameruntime/gameruntime.hpp"
#include "runtime/modules/storage.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <optional>

// XGameSave: a user's saved games, as containers of named blobs. On the console they sync
// with the cloud; here they are files under the title's save folder, in the same layout an
// ERA title's connected storage has. XGameSaveFiles gives the title a folder to use as it
// likes.
namespace xv::runtime::modules::gameruntime {
namespace {
constexpr HRESULT e_buffer_too_small = static_cast<HRESULT>(0x80830007);
constexpr HRESULT e_blob_not_found = static_cast<HRESULT>(0x80830008);
const char provider_identity[] = "XGameSaveInitializeProviderAsync";
const char quota_identity[] = "XGameSaveGetRemainingQuotaAsync";
const char delete_identity[] = "XGameSaveDeleteContainerAsync";
const char read_identity[] = "XGameSaveReadBlobDataAsync";
const char submit_identity[] = "XGameSaveSubmitUpdateAsync";
const char folder_identity[] = "XGameSaveFilesGetFolderWithUiAsync";
constexpr std::int64_t quota = 256ll << 20;

std::filesystem::path save_root() {
    auto root = save_directory();
    if (root.empty())
        root = std::filesystem::temp_directory_path() / "xvalkyrie-saves";
    return root;
}
struct Provider {
    SaveSpace space{save_root() / "gamesave"};
};
struct Container {
    Provider *provider;
    std::string name;
};
struct Update {
    Container *container;
    std::optional<std::string> display_name;
    std::map<std::string, std::vector<std::uint8_t>> writes;
    std::vector<std::string> deletes;
};
// XGameSaveContainerInfo, XGameSaveBlobInfo and XGameSaveBlob
struct ContainerInfo {
    const char *name, *display_name;
    std::uint32_t blob_count;
    std::uint64_t total_size;
    std::int64_t last_modified;
    bool needs_sync;
};
struct BlobInfo {
    const char *name;
    std::uint32_t size;
};
struct Blob {
    BlobInfo info;
    std::uint8_t *data;
};
using ContainerCallback = bool CALLBACK(const ContainerInfo *info, void *context);
using BlobCallback = bool CALLBACK(const BlobInfo *info, void *context);

template <class Value> std::vector<std::uint8_t> bytes_of(const Value &value) {
    std::vector<std::uint8_t> bytes(sizeof value);
    std::memcpy(bytes.data(), &value, sizeof value);
    return bytes;
}
template <class Action> HRESULT guarded(Action action) {
    try {
        return action();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[Runtime] XGameSave: %s\n", e.what());
        return E_FAIL;
    }
}

HRESULT WINAPI initialize_provider(void *, void *, const char *, bool, Provider **provider) {
    if (!provider)
        return E_POINTER;
    static bool shown = false;
    if (!std::exchange(shown, true))
        std::fprintf(stderr, "[Runtime] XGameSave: saved games in %ls\n",
                     (save_root() / "gamesave").c_str());
    *provider = new Provider;
    return S_OK;
}
HRESULT WINAPI initialize_provider_async(void *table, void *user, const char *configuration,
                                         bool on_demand, XAsyncBlock *block) {
    Provider *provider = nullptr;
    const auto status = initialize_provider(table, user, configuration, on_demand, &provider);
    return finish(block, provider_identity, status, bytes_of(provider));
}
HRESULT WINAPI initialize_provider_result(void *, XAsyncBlock *block, Provider **provider) {
    return result(block, provider_identity, sizeof *provider, provider, nullptr);
}
void WINAPI close_provider(void *, Provider *provider) { delete provider; }
HRESULT WINAPI remaining_quota(void *, Provider *provider, std::int64_t *remaining) {
    if (!provider || !remaining)
        return E_INVALIDARG;
    return guarded([&] {
        std::int64_t used = 0;
        for (const auto &container : provider->space.containers())
            used += static_cast<std::int64_t>(container.size);
        *remaining = std::max<std::int64_t>(quota - used, 0);
        return S_OK;
    });
}
HRESULT WINAPI remaining_quota_async(void *table, Provider *provider, XAsyncBlock *block) {
    std::int64_t remaining = 0;
    const auto status = remaining_quota(table, provider, &remaining);
    return finish(block, quota_identity, status, bytes_of(remaining));
}
HRESULT WINAPI remaining_quota_result(void *, XAsyncBlock *block, std::int64_t *remaining) {
    return result(block, quota_identity, sizeof *remaining, remaining, nullptr);
}
HRESULT WINAPI delete_container(void *, Provider *provider, const char *name) {
    if (!provider || !name)
        return E_INVALIDARG;
    return guarded([&] {
        provider->space.remove(name);
        return S_OK;
    });
}
HRESULT WINAPI delete_container_async(void *table, Provider *provider, const char *name,
                                      XAsyncBlock *block) {
    return finish(block, delete_identity, delete_container(table, provider, name));
}
HRESULT WINAPI delete_container_result(void *, XAsyncBlock *block) {
    return result(block, delete_identity, 0, nullptr, nullptr);
}
// the containers whose name starts with `prefix`, or the one called exactly that
HRESULT report_containers(Provider *provider, const std::string &prefix, bool exact, void *context,
                          ContainerCallback *callback) {
    if (!provider || !callback)
        return E_INVALIDARG;
    return guarded([&] {
        for (const auto &container : provider->space.containers(prefix)) {
            if (exact && container.name != prefix)
                continue;
            const auto display =
                container.display_name.empty() ? container.name : container.display_name;
            const ContainerInfo info{
                container.name.c_str(),
                display.c_str(),
                static_cast<std::uint32_t>(provider->space.blobs(container.name).size()),
                container.size,
                std::chrono::system_clock::to_time_t(container.modified),
                false};
            if (!callback(&info, context))
                break;
        }
        return S_OK;
    });
}
HRESULT WINAPI container_info(void *, Provider *provider, const char *name, void *context,
                              ContainerCallback *callback) {
    return name ? report_containers(provider, name, true, context, callback) : E_INVALIDARG;
}
HRESULT WINAPI enumerate_containers(void *, Provider *provider, void *context,
                                    ContainerCallback *callback) {
    return report_containers(provider, {}, false, context, callback);
}
HRESULT WINAPI enumerate_containers_by_name(void *, Provider *provider, const char *prefix,
                                            void *context, ContainerCallback *callback) {
    return report_containers(provider, prefix ? prefix : "", false, context, callback);
}
HRESULT WINAPI create_container(void *, Provider *provider, const char *name,
                                Container **container) {
    if (!provider || !name || !container)
        return E_INVALIDARG;
    *container = new Container{provider, name};
    return S_OK;
}
void WINAPI close_container(void *, Container *container) { delete container; }
HRESULT report_blobs(Container *container, const std::string &prefix, void *context,
                     BlobCallback *callback) {
    if (!container || !callback)
        return E_INVALIDARG;
    return guarded([&] {
        for (const auto &blob : container->provider->space.blobs(container->name, prefix)) {
            const BlobInfo info{blob.name.c_str(), static_cast<std::uint32_t>(blob.size)};
            if (!callback(&info, context))
                break;
        }
        return S_OK;
    });
}
HRESULT WINAPI enumerate_blobs(void *, Container *container, void *context,
                               BlobCallback *callback) {
    return report_blobs(container, {}, context, callback);
}
HRESULT WINAPI enumerate_blobs_by_name(void *, Container *container, const char *prefix,
                                       void *context, BlobCallback *callback) {
    return report_blobs(container, prefix ? prefix : "", context, callback);
}
// The blobs asked for (all of them without names) as the title gets them: the array of
// XGameSaveBlob, then each blob's name and data. The pointers are offsets from the start
// until the block is where the title reads it.
HRESULT pack_blobs(Container *container, const char **names, std::uint32_t count,
                   std::vector<std::uint8_t> &packed, std::uint32_t &packed_count) {
    if (!container)
        return E_INVALIDARG;
    return guarded([&] {
        std::vector<std::string> wanted;
        if (names)
            for (std::uint32_t i = 0; i < count; ++i)
                wanted.emplace_back(names[i] ? names[i] : "");
        else
            for (const auto &blob : container->provider->space.blobs(container->name))
                wanted.push_back(blob.name);
        std::vector<std::vector<std::uint8_t>> contents;
        std::size_t size = wanted.size() * sizeof(Blob);
        for (const auto &name : wanted) {
            auto data = container->provider->space.read(container->name, name);
            if (!data)
                return e_blob_not_found;
            size += name.size() + 1 + data->size();
            contents.push_back(std::move(*data));
        }
        packed.assign(size, 0);
        std::size_t at = wanted.size() * sizeof(Blob);
        for (std::size_t i = 0; i < wanted.size(); ++i) {
            Blob blob{};
            blob.info.name = reinterpret_cast<const char *>(at);
            blob.info.size = static_cast<std::uint32_t>(contents[i].size());
            std::memcpy(packed.data() + at, wanted[i].c_str(), wanted[i].size() + 1);
            at += wanted[i].size() + 1;
            blob.data = reinterpret_cast<std::uint8_t *>(at);
            std::memcpy(packed.data() + at, contents[i].data(), contents[i].size());
            at += contents[i].size();
            std::memcpy(packed.data() + i * sizeof(Blob), &blob, sizeof blob);
        }
        packed_count = static_cast<std::uint32_t>(wanted.size());
        return S_OK;
    });
}
void rebase(Blob *blobs, std::uint32_t count) {
    const auto base = reinterpret_cast<std::uintptr_t>(blobs);
    for (std::uint32_t i = 0; i < count; ++i) {
        blobs[i].info.name += base;
        blobs[i].data += base;
    }
}
HRESULT WINAPI read_blobs(void *, Container *container, const char **names, std::uint32_t *count,
                          std::size_t size, Blob *blobs) {
    if (!count || !blobs)
        return E_INVALIDARG;
    std::vector<std::uint8_t> packed;
    std::uint32_t packed_count = 0;
    if (const auto status = pack_blobs(container, names, *count, packed, packed_count);
        FAILED(status))
        return status;
    if (size < packed.size())
        return e_buffer_too_small;
    std::memcpy(blobs, packed.data(), packed.size());
    rebase(blobs, packed_count);
    *count = packed_count;
    return S_OK;
}
HRESULT WINAPI read_blobs_async(void *, Container *container, const char **names,
                                std::uint32_t count, XAsyncBlock *block) {
    std::vector<std::uint8_t> packed;
    std::uint32_t packed_count = 0;
    const auto status = pack_blobs(container, names, count, packed, packed_count);
    return finish(block, read_identity, status, std::move(packed));
}
HRESULT WINAPI read_blobs_result(void *, XAsyncBlock *block, std::size_t size, Blob *blobs,
                                 std::uint32_t *count) {
    if (count)
        *count = 0;
    std::size_t used = 0;
    const auto status = result(block, read_identity, size, blobs, &used);
    if (FAILED(status) || !used)
        return status == e_not_sufficient_buffer ? e_buffer_too_small : status;
    // the first name follows the array
    const auto blobs_packed = static_cast<std::uint32_t>(
        reinterpret_cast<std::uintptr_t>(blobs[0].info.name) / sizeof(Blob));
    rebase(blobs, blobs_packed);
    if (count)
        *count = blobs_packed;
    return status;
}
HRESULT WINAPI create_update(void *, Container *container, const char *display_name,
                             Update **update) {
    if (!container || !update)
        return E_INVALIDARG;
    *update = new Update{container, {}, {}, {}};
    if (display_name)
        (*update)->display_name = display_name;
    return S_OK;
}
void WINAPI close_update(void *, Update *update) { delete update; }
HRESULT WINAPI submit_blob_write(void *, Update *update, const char *name, const std::uint8_t *data,
                                 std::size_t bytes) {
    if (!update || !name || (!data && bytes))
        return E_INVALIDARG;
    update->writes[name].assign(data, data + bytes);
    return S_OK;
}
HRESULT WINAPI submit_blob_delete(void *, Update *update, const char *name) {
    if (!update || !name)
        return E_INVALIDARG;
    update->deletes.emplace_back(name);
    return S_OK;
}
HRESULT WINAPI submit_update(void *, Update *update) {
    if (!update)
        return E_INVALIDARG;
    return guarded([&] {
        update->container->provider->space.submit(update->container->name, update->writes,
                                                  update->deletes, update->display_name);
        return S_OK;
    });
}
HRESULT WINAPI submit_update_async(void *table, Update *update, XAsyncBlock *block) {
    return finish(block, submit_identity, submit_update(table, update));
}
HRESULT WINAPI submit_update_result(void *, XAsyncBlock *block) {
    return result(block, submit_identity, 0, nullptr, nullptr);
}

std::string files_folder() {
    const auto folder = save_root() / "files";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    const auto text = folder.u8string();
    return std::string(text.begin(), text.end()) + "\\";
}
HRESULT WINAPI files_folder_async(void *, void *, const char *, XAsyncBlock *block) {
    const auto folder = files_folder();
    return finish(block, folder_identity, S_OK,
                  std::vector<std::uint8_t>(folder.c_str(), folder.c_str() + folder.size() + 1));
}
HRESULT WINAPI files_folder_result(void *, XAsyncBlock *block, std::size_t size, char *folder) {
    return result(block, folder_identity, size, folder, nullptr);
}
HRESULT WINAPI files_remaining_quota(void *, void *, const char *, std::int64_t *remaining) {
    if (!remaining)
        return E_POINTER;
    *remaining = quota;
    return S_OK;
}
} // namespace

void install_save() {
    implement("XGameSaveInitializeProvider", &initialize_provider);
    implement("XGameSaveInitializeProviderAsync", &initialize_provider_async);
    implement("XGameSaveInitializeProviderResult", &initialize_provider_result);
    implement("XGameSaveCloseProvider", &close_provider);
    implement("XGameSaveGetRemainingQuota", &remaining_quota);
    implement("XGameSaveGetRemainingQuotaAsync", &remaining_quota_async);
    implement("XGameSaveGetRemainingQuotaResult", &remaining_quota_result);
    implement("XGameSaveDeleteContainer", &delete_container);
    implement("XGameSaveDeleteContainerAsync", &delete_container_async);
    implement("XGameSaveDeleteContainerResult", &delete_container_result);
    implement("XGameSaveGetContainerInfo", &container_info);
    implement("XGameSaveEnumerateContainerInfo", &enumerate_containers);
    implement("XGameSaveEnumerateContainerInfoByName", &enumerate_containers_by_name);
    implement("XGameSaveCreateContainer", &create_container);
    implement("XGameSaveCloseContainer", &close_container);
    implement("XGameSaveEnumerateBlobInfo", &enumerate_blobs);
    implement("XGameSaveEnumerateBlobInfoByName", &enumerate_blobs_by_name);
    implement("XGameSaveReadBlobData", &read_blobs);
    implement("XGameSaveReadBlobDataAsync", &read_blobs_async);
    implement("XGameSaveReadBlobDataResult", &read_blobs_result);
    implement("XGameSaveCreateUpdate", &create_update);
    implement("XGameSaveCloseUpdate", &close_update);
    implement("XGameSaveSubmitBlobWrite", &submit_blob_write);
    implement("XGameSaveSubmitBlobDelete", &submit_blob_delete);
    implement("XGameSaveSubmitUpdate", &submit_update);
    implement("XGameSaveSubmitUpdateAsync", &submit_update_async);
    implement("XGameSaveSubmitUpdateResult", &submit_update_result);
    implement("XGameSaveFilesGetFolderWithUiAsync", &files_folder_async);
    implement("XGameSaveFilesGetFolderWithUiResult", &files_folder_result);
    implement("XGameSaveFilesGetRemainingQuota", &files_remaining_quota);
}
} // namespace xv::runtime::modules::gameruntime
