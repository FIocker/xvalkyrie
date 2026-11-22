#include "runtime/modules/gameruntime/gameruntime.hpp"
#include <atomic>
#include <cstring>
#include <ctime>

// XStore without a store: the title is owned outright, on a full license that does not
// expire; it has no add-ons, no consumables and no updates waiting, and nothing can be
// bought. What needs the store's service (collection and purchase ids, license tokens)
// fails the way it does on a console that is offline. Structures are those of XStore.h.
namespace xv::runtime::modules::gameruntime {
namespace {
struct Context {
} context;
struct ProductsQuery {
} products_query;
struct License {
} license;
std::atomic<std::uint64_t> tokens{0};
constexpr HRESULT e_offline = static_cast<HRESULT>(0x800704CF);   // ERROR_NETWORK_UNREACHABLE
constexpr HRESULT e_cancelled = static_cast<HRESULT>(0x80004004); // E_ABORT: the user left
const char products_identity[] = "XStoreQueryProducts";
const char license_identity[] = "XStoreQueryGameLicenseAsync";
const char package_license_identity[] = "XStoreAcquireLicenseForPackageAsync";
const char can_acquire_identity[] = "XStoreCanAcquireLicense";
const char count_identity[] = "XStoreQueryCount";
const char balance_identity[] = "XStoreQueryConsumableBalanceRemainingAsync";
const char service_identity[] = "XStoreService";
const char ui_identity[] = "XStoreShowUI";

struct GameLicense {
    char sku_store_id[18];
    bool is_active, is_trial_owned_by_this_user, is_disc_license, is_trial;
    std::uint32_t trial_time_remaining_in_seconds;
    char trial_unique_id[64];
    std::time_t expiration_date;
};
struct CanAcquireLicenseResult {
    char licensable_sku[18];
    std::uint32_t status; // XStoreCanLicenseStatus: not licensable, licensable, not found
};

template <class Value> std::vector<std::uint8_t> bytes_of(const Value &value) {
    std::vector<std::uint8_t> bytes(sizeof value);
    std::memcpy(bytes.data(), &value, sizeof value);
    return bytes;
}

HRESULT WINAPI create_context(void *, void *, Context **handle) {
    if (!handle)
        return E_POINTER;
    *handle = &context;
    return S_OK;
}
void WINAPI close_handle(void *, void *) {}

// every query of products finds none: one page, empty
HRESULT WINAPI query_associated(void *, Context *, std::uint32_t, std::uint32_t,
                                XAsyncBlock *block) {
    return finish(block, products_identity, S_OK, bytes_of(&products_query));
}
HRESULT WINAPI query_products(void *, Context *, std::uint32_t, const char **, std::size_t,
                              const char **, std::size_t, XAsyncBlock *block) {
    return finish(block, products_identity, S_OK, bytes_of(&products_query));
}
HRESULT WINAPI query_for_game(void *, Context *, XAsyncBlock *block) {
    return finish(block, products_identity, S_OK, bytes_of(&products_query));
}
HRESULT WINAPI query_for_package(void *, Context *, std::uint32_t, const char *,
                                 XAsyncBlock *block) {
    return finish(block, products_identity, S_OK, bytes_of(&products_query));
}
HRESULT WINAPI query_next_page(void *, ProductsQuery *, XAsyncBlock *block) {
    return finish(block, products_identity, S_OK, bytes_of(&products_query));
}
HRESULT WINAPI query_products_result(void *, XAsyncBlock *block, ProductsQuery **query) {
    return result(block, products_identity, sizeof *query, query, nullptr);
}
HRESULT WINAPI enumerate_products(void *, ProductsQuery *, void *, void *) { return S_OK; }
bool WINAPI has_more_pages(void *, ProductsQuery *) { return false; }

HRESULT WINAPI query_game_license(void *, Context *, XAsyncBlock *block) {
    GameLicense owned{};
    owned.is_active = true;
    owned.expiration_date = static_cast<std::time_t>(0x7fffffffffffffffll);
    return finish(block, license_identity, S_OK, bytes_of(owned));
}
HRESULT WINAPI query_game_license_result(void *, XAsyncBlock *block, GameLicense *owned) {
    return result(block, license_identity, sizeof *owned, owned, nullptr);
}
// the license of a package of the title's: held, and never lost
HRESULT WINAPI acquire_license(void *, Context *, const char *, XAsyncBlock *block) {
    return finish(block, package_license_identity, S_OK, bytes_of(&license));
}
HRESULT WINAPI acquire_license_result(void *, XAsyncBlock *block, License **handle) {
    return result(block, package_license_identity, sizeof *handle, handle, nullptr);
}
bool WINAPI license_valid(void *, License *) { return true; }
// what is not installed cannot be licensed either: there is nowhere to get it from
HRESULT WINAPI can_acquire(void *, Context *, const char *, XAsyncBlock *block) {
    return finish(block, can_acquire_identity, S_OK, bytes_of(CanAcquireLicenseResult{{}, 0}));
}
HRESULT WINAPI can_acquire_result(void *, XAsyncBlock *block, CanAcquireLicenseResult *answer) {
    return result(block, can_acquire_identity, sizeof *answer, answer, nullptr);
}

// add-on licenses and package updates: lists of nothing
HRESULT WINAPI query_none(void *, Context *, XAsyncBlock *block) {
    return finish(block, count_identity, S_OK);
}
HRESULT WINAPI query_none_count(void *, XAsyncBlock *block, std::uint32_t *count) {
    if (count)
        *count = 0;
    return result(block, count_identity, 0, nullptr, nullptr);
}
HRESULT WINAPI query_none_result(void *, XAsyncBlock *block, std::uint32_t, void *) {
    return result(block, count_identity, 0, nullptr, nullptr);
}
HRESULT WINAPI consumable_balance(void *, Context *, const char *, XAsyncBlock *block) {
    return finish(block, balance_identity, S_OK, bytes_of(std::uint32_t{0}));
}
HRESULT WINAPI consumable_balance_result(void *, XAsyncBlock *block, std::uint32_t *quantity) {
    return result(block, balance_identity, sizeof *quantity, quantity, nullptr);
}

// The store's service is not reached. In all of these the block is the last argument, after
// the context and two or three pointers.
HRESULT WINAPI service_2(void *, Context *, const void *, XAsyncBlock *block) {
    return finish(block, service_identity, e_offline);
}
HRESULT WINAPI service_3(void *, Context *, const void *, const void *, XAsyncBlock *block) {
    return finish(block, service_identity, e_offline);
}
HRESULT WINAPI service_4(void *, Context *, const void *, const void *, const void *,
                         XAsyncBlock *block) {
    return finish(block, service_identity, e_offline);
}
HRESULT WINAPI service_result_size(void *, XAsyncBlock *block, std::size_t *size) {
    return result_size(block, size);
}
HRESULT WINAPI service_result(void *, XAsyncBlock *block) {
    return result(block, service_identity, 0, nullptr, nullptr);
}
// the store's own screens: the user comes back without having bought anything
HRESULT WINAPI show_ui_1(void *, Context *, XAsyncBlock *block) {
    return finish(block, ui_identity, e_cancelled);
}
HRESULT WINAPI show_ui_2(void *, Context *, const void *, XAsyncBlock *block) {
    return finish(block, ui_identity, e_cancelled);
}
HRESULT WINAPI show_redeem_ui(void *, Context *, const char *, const char **, std::size_t, bool,
                              XAsyncBlock *block) {
    return finish(block, ui_identity, e_cancelled);
}
HRESULT WINAPI show_purchase_ui(void *, Context *, const char *, const char *, const char *,
                                XAsyncBlock *block) {
    return finish(block, ui_identity, e_cancelled);
}
HRESULT WINAPI show_ui_result(void *, XAsyncBlock *block) {
    return result(block, ui_identity, 0, nullptr, nullptr);
}

// a license that never changes and is never lost: a token, and no event
HRESULT WINAPI register_event(void *, void *, void *, void *, void *, std::uint64_t *token) {
    if (!token)
        return E_POINTER;
    *token = ++tokens;
    return S_OK;
}
bool WINAPI unregister_event(void *, void *, std::uint64_t, bool) { return true; }
bool WINAPI availability_purchasable(void *, std::uint64_t) { return false; }
} // namespace

void install_store() {
    implement("XStoreCreateContext", &create_context);
    implement("XStoreCloseContextHandle", &close_handle);
    implement("XStoreQueryAssociatedProductsAsync", &query_associated);
    implement("XStoreQueryEntitledProductsAsync", &query_associated);
    implement("XStoreQueryProductsAsync", &query_products);
    implement("XStoreQueryProductForCurrentGameAsync", &query_for_game);
    implement("XStoreQueryProductForPackageAsync", &query_for_package);
    implement("XStoreProductsQueryNextPageAsync", &query_next_page);
    for (const auto *name :
         {"XStoreQueryAssociatedProductsResult", "XStoreQueryEntitledProductsResult",
          "XStoreQueryProductsResult", "XStoreQueryProductForCurrentGameResult",
          "XStoreQueryProductForPackageResult", "XStoreProductsQueryNextPageResult"})
        implement(name, &query_products_result);
    implement("XStoreEnumerateProductsQuery", &enumerate_products);
    implement("XStoreProductsQueryHasMorePages", &has_more_pages);
    implement("XStoreCloseProductsQueryHandle", &close_handle);
    implement("XStoreQueryGameLicenseAsync", &query_game_license);
    implement("XStoreQueryGameLicenseResult", &query_game_license_result);
    implement("XStoreAcquireLicenseForPackageAsync", &acquire_license);
    implement("XStoreAcquireLicenseForDurablesAsync", &acquire_license);
    implement("XStoreAcquireLicenseForPackageResult", &acquire_license_result);
    implement("XStoreAcquireLicenseForDurablesResult", &acquire_license_result);
    implement("XStoreIsLicenseValid", &license_valid);
    implement("XStoreCloseLicenseHandle", &close_handle);
    implement("XStoreCanAcquireLicenseForStoreIdAsync", &can_acquire);
    implement("XStoreCanAcquireLicenseForPackageAsync", &can_acquire);
    implement("XStoreCanAcquireLicenseForStoreIdResult", &can_acquire_result);
    implement("XStoreCanAcquireLicenseForPackageResult", &can_acquire_result);
    implement("XStoreQueryAddOnLicensesAsync", &query_none);
    implement("XStoreQueryAddOnLicensesResultCount", &query_none_count);
    implement("XStoreQueryAddOnLicensesResult", &query_none_result);
    implement("XStoreQueryGameAndDlcPackageUpdatesAsync", &query_none);
    implement("XStoreQueryGameAndDlcPackageUpdatesResultCount", &query_none_count);
    implement("XStoreQueryGameAndDlcPackageUpdatesResult", &query_none_result);
    implement("XStoreQueryConsumableBalanceRemainingAsync", &consumable_balance);
    implement("XStoreQueryConsumableBalanceRemainingResult", &consumable_balance_result);
    implement("XStoreGetUserCollectionsIdAsync", &service_3);
    implement("XStoreGetUserPurchaseIdAsync", &service_3);
    implement("XStoreQueryLicenseTokenAsync", &service_4);
    implement("XStoreReportConsumableFulfillmentAsync", &service_4);
    for (const auto *name :
         {"XStoreGetUserCollectionsIdResultSize", "XStoreGetUserPurchaseIdResultSize",
          "XStoreQueryLicenseTokenResultSize"})
        implement(name, &service_result_size);
    for (const auto *name :
         {"XStoreGetUserCollectionsIdResult", "XStoreGetUserPurchaseIdResult",
          "XStoreQueryLicenseTokenResult", "XStoreReportConsumableFulfillmentResult"})
        implement(name, &service_result);
    implement("XStoreShowPurchaseUIAsync", &show_purchase_ui);
    implement("XStoreShowRateAndReviewUIAsync", &show_ui_1);
    implement("XStoreShowRedeemTokenUIAsync", &show_redeem_ui);
    implement("XStoreShowProductPageUIAsync", &show_ui_2);
    for (const auto *name : {"XStoreShowPurchaseUIResult", "XStoreShowRateAndReviewUIResult",
                             "XStoreShowRedeemTokenUIResult", "XStoreShowProductPageUIResult"})
        implement(name, &show_ui_result);
    implement("XStoreRegisterGameLicenseChanged", &register_event);
    implement("XStoreUnregisterGameLicenseChanged", &unregister_event);
    implement("XStoreRegisterPackageLicenseLost", &register_event);
    implement("XStoreUnregisterPackageLicenseLost", &unregister_event);
    implement("XStoreIsAvailabilityPurchasable", &availability_purchasable);
}
} // namespace xv::runtime::modules::gameruntime
