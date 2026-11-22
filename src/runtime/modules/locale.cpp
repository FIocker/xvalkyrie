#include "runtime/modules/locale.hpp"
#include "runtime/common/runtime.hpp"
#include <cstdio>
#include <cwchar>
#include <windows.h>

// The language of the console is a setting of its own, not the host user's. What is answered
// here are the functions a title asks for the user's or the system's language with; what it
// does with a locale it names itself is the host's to answer.
namespace xv::runtime::modules {
namespace {
struct Language {
    std::wstring name;
    bool set = false; // not the host's
    LCID lcid = LOCALE_USER_DEFAULT;
};
const Language &language() {
    static const Language chosen = [] {
        Language result;
        const auto &wanted = console_settings().language;
        if (!wanted.empty() && IsValidLocaleName(wanted.c_str())) {
            result = {wanted, true, LocaleNameToLCID(wanted.c_str(), 0)};
            // what the host works out from the process's languages follows as well
            const auto list = wanted + L'\0';
            ULONG count = 0;
            SetProcessPreferredUILanguages(MUI_LANGUAGE_NAME, list.c_str(), &count);
        } else {
            wchar_t host[LOCALE_NAME_MAX_LENGTH] = L"en-US";
            GetUserDefaultLocaleName(host, LOCALE_NAME_MAX_LENGTH);
            result.name = host;
        }
        return result;
    }();
    return chosen;
}
int copy(const std::wstring &text, wchar_t *buffer, int size) {
    const int needed = static_cast<int>(text.size()) + 1;
    if (!buffer || size < needed) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    std::wmemcpy(buffer, text.c_str(), static_cast<std::size_t>(needed));
    return needed;
}
int WINAPI user_locale_name(wchar_t *name, int size) {
    return language().set ? copy(language().name, name, size)
                          : GetUserDefaultLocaleName(name, size);
}
int WINAPI system_locale_name(wchar_t *name, int size) {
    return language().set ? copy(language().name, name, size)
                          : GetSystemDefaultLocaleName(name, size);
}
LCID WINAPI user_lcid() { return language().set ? language().lcid : GetUserDefaultLCID(); }
LCID WINAPI system_lcid() { return language().set ? language().lcid : GetSystemDefaultLCID(); }
LCID WINAPI thread_locale() { return language().set ? language().lcid : GetThreadLocale(); }
LANGID WINAPI user_language() {
    return language().set ? LANGIDFROMLCID(language().lcid) : GetUserDefaultLangID();
}
LANGID WINAPI system_language() {
    return language().set ? LANGIDFROMLCID(language().lcid) : GetSystemDefaultLangID();
}
LANGID WINAPI user_ui_language() {
    return language().set ? LANGIDFROMLCID(language().lcid) : GetUserDefaultUILanguage();
}
LANGID WINAPI system_ui_language() {
    return language().set ? LANGIDFROMLCID(language().lcid) : GetSystemDefaultUILanguage();
}
LANGID WINAPI thread_ui_language() {
    return language().set ? LANGIDFROMLCID(language().lcid) : GetThreadUILanguage();
}
// one language, as its name or as the four hexadecimal digits of its identifier, and the
// empty string that ends the list
BOOL preferred(DWORD flags, ULONG *count, wchar_t *buffer, ULONG *size) {
    if (!count || !size) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    auto entry = language().name;
    if (flags & MUI_LANGUAGE_ID) {
        wchar_t id[8];
        std::swprintf(id, 8, L"%04X", LANGIDFROMLCID(language().lcid));
        entry = id;
    }
    const auto needed = static_cast<ULONG>(entry.size() + 2);
    const bool fits = buffer && *size >= needed;
    if (buffer && !fits) {
        *size = needed;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    if (fits) {
        std::wmemcpy(buffer, entry.c_str(), entry.size() + 1);
        buffer[entry.size() + 1] = L'\0';
    }
    *count = 1;
    *size = needed;
    return TRUE;
}
BOOL WINAPI user_preferred(DWORD flags, ULONG *count, wchar_t *buffer, ULONG *size) {
    return language().set ? preferred(flags, count, buffer, size)
                          : GetUserPreferredUILanguages(flags, count, buffer, size);
}
BOOL WINAPI system_preferred(DWORD flags, ULONG *count, wchar_t *buffer, ULONG *size) {
    return language().set ? preferred(flags, count, buffer, size)
                          : GetSystemPreferredUILanguages(flags, count, buffer, size);
}
BOOL WINAPI thread_preferred(DWORD flags, ULONG *count, wchar_t *buffer, ULONG *size) {
    return language().set ? preferred(flags, count, buffer, size)
                          : GetThreadPreferredUILanguages(flags, count, buffer, size);
}
BOOL WINAPI process_preferred(DWORD flags, ULONG *count, wchar_t *buffer, ULONG *size) {
    return language().set ? preferred(flags, count, buffer, size)
                          : GetProcessPreferredUILanguages(flags, count, buffer, size);
}
// the user's or the system's default locale, by name or by identifier, is the console's
int WINAPI locale_info_ex(const wchar_t *name, LCTYPE type, wchar_t *data, int size) {
    if (language().set && (!name || !std::wcscmp(name, LOCALE_NAME_SYSTEM_DEFAULT)))
        name = language().name.c_str();
    return GetLocaleInfoEx(name, type, data, size);
}
LCID console_lcid(LCID lcid) {
    return language().set && (lcid == LOCALE_USER_DEFAULT || lcid == LOCALE_SYSTEM_DEFAULT ||
                              lcid == LOCALE_NEUTRAL)
               ? language().lcid
               : lcid;
}
int WINAPI locale_info_w(LCID lcid, LCTYPE type, wchar_t *data, int size) {
    return GetLocaleInfoW(console_lcid(lcid), type, data, size);
}
int WINAPI locale_info_a(LCID lcid, LCTYPE type, char *data, int size) {
    return GetLocaleInfoA(console_lcid(lcid), type, data, size);
}
// the region of the language: "AR" of es-AR
int WINAPI user_geo_name(wchar_t *name, int size) {
    if (!language().set)
        return GetUserDefaultGeoName(name, size);
    wchar_t region[16] = L"";
    if (!GetLocaleInfoEx(language().name.c_str(), LOCALE_SISO3166CTRYNAME, region, 16))
        return GetUserDefaultGeoName(name, size);
    if (!name || !size)
        return static_cast<int>(std::wcslen(region)) + 1;
    return copy(region, name, size);
}
GEOID WINAPI user_geo_id(GEOCLASS geo_class) {
    GEOID id = 0;
    if (language().set && geo_class == GEOCLASS_NATION &&
        GetLocaleInfoEx(language().name.c_str(), LOCALE_IGEOID | LOCALE_RETURN_NUMBER,
                        reinterpret_cast<wchar_t *>(&id), sizeof id / sizeof(wchar_t)))
        return id;
    return GetUserGeoID(geo_class);
}
} // namespace

const std::wstring &console_language() { return language().name; }

void install_locale(Registry &registry) {
    const auto add = [&](const char *name, auto *function) {
        registry.add_api(name, reinterpret_cast<void *>(function));
    };
    add("GetUserDefaultLocaleName", &user_locale_name);
    add("GetSystemDefaultLocaleName", &system_locale_name);
    add("GetUserDefaultLCID", &user_lcid);
    add("GetSystemDefaultLCID", &system_lcid);
    add("GetThreadLocale", &thread_locale);
    add("GetUserDefaultLangID", &user_language);
    add("GetSystemDefaultLangID", &system_language);
    add("GetUserDefaultUILanguage", &user_ui_language);
    add("GetSystemDefaultUILanguage", &system_ui_language);
    add("GetThreadUILanguage", &thread_ui_language);
    add("GetUserPreferredUILanguages", &user_preferred);
    add("GetSystemPreferredUILanguages", &system_preferred);
    add("GetThreadPreferredUILanguages", &thread_preferred);
    add("GetProcessPreferredUILanguages", &process_preferred);
    add("GetLocaleInfoEx", &locale_info_ex);
    add("GetLocaleInfoW", &locale_info_w);
    add("GetLocaleInfoA", &locale_info_a);
    add("GetUserDefaultGeoName", &user_geo_name);
    add("GetUserGeoID", &user_geo_id);
}
} // namespace xv::runtime::modules
