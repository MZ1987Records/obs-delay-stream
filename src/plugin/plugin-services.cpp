#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "plugin/plugin-services.hpp"

#include <winsock2.h>
#include <windows.h>
#include <psapi.h>

#include <obs-module.h>

#include "plugin/plugin-utils.hpp"

namespace plugin_main_obs_services {

namespace {

template <typename Fn>
Fn find_obs_symbol(const char* symbol_name) {
    if (!symbol_name || !*symbol_name) return nullptr;
    const char* modules[] = {
        "obs-frontend-api.dll",
        "obs-frontend-api64.dll",
        "obs-frontend-api",
        "obs64.exe",
        "obs64",
        "obs.dll",
        "libobs.dll",
        "libobs",
        nullptr
    };
    for (int i = 0; modules[i]; ++i) {
        HMODULE mod = GetModuleHandleA(modules[i]);
        if (!mod) continue;
        FARPROC p = GetProcAddress(mod, symbol_name);
        if (p) return reinterpret_cast<Fn>(p);
    }
    for (int i = 0; modules[i]; ++i) {
        HMODULE mod = LoadLibraryA(modules[i]);
        if (!mod) continue;
        FARPROC p = GetProcAddress(mod, symbol_name);
        if (p) return reinterpret_cast<Fn>(p);
    }

    HMODULE mods[1024];
    DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        const DWORD count = needed / sizeof(HMODULE);
        for (DWORD i = 0; i < count; ++i) {
            FARPROC p = GetProcAddress(mods[i], symbol_name);
            if (p) return reinterpret_cast<Fn>(p);
        }
    }
    return nullptr;
}

} // namespace

bool is_obs_source_removed(obs_source_t* source) {
    using source_removed_fn = bool (*)(obs_source_t*);
    static source_removed_fn fn =
        find_obs_symbol<source_removed_fn>("obs_source_removed");
    if (!source || !fn) return false;
    return fn(source);
}

std::string get_obs_stream_url() {
    using get_streaming_service_fn = obs_service_t* (*)();
    using service_get_settings_fn = obs_data_t* (*)(obs_service_t*);
    using service_get_ref_fn = obs_service_t* (*)(obs_service_t*);
    using service_addref_fn = void (*)(obs_service_t*);
    using service_get_url_fn = const char* (*)(obs_service_t*);
    using service_release_fn = void (*)(obs_service_t*);

    auto get_streaming_service =
        find_obs_symbol<get_streaming_service_fn>("obs_frontend_get_streaming_service");
    auto service_get_settings =
        find_obs_symbol<service_get_settings_fn>("obs_service_get_settings");
    auto service_get_ref =
        find_obs_symbol<service_get_ref_fn>("obs_service_get_ref");
    auto service_addref =
        find_obs_symbol<service_addref_fn>("obs_service_addref");
    auto service_get_url =
        find_obs_symbol<service_get_url_fn>("obs_service_get_url");
    auto service_release =
        find_obs_symbol<service_release_fn>("obs_service_release");
    if (!get_streaming_service || !service_get_settings) return "";

    obs_service_t* borrowed = get_streaming_service();
    if (!borrowed) return "";

    obs_service_t* owned = nullptr;
    if (service_get_ref) {
        owned = service_get_ref(borrowed);
    } else if (service_addref && service_release) {
        service_addref(borrowed);
        owned = borrowed;
    }
    obs_service_t* service = owned ? owned : borrowed;

    std::string url;
    std::string stream_key;
    if (service_get_url) {
        url = plugin_main_utils::normalize_rtmp_url_candidate(service_get_url(service));
    }

    obs_data_t* settings = service_get_settings(service);
    if (settings) {
        if (url.empty()) {
            const char* keys[] = {
                "server",
                "url",
                "ingest_url",
                "server_url",
                "rtmp_url",
                nullptr
            };
            for (int i = 0; keys[i]; ++i) {
                url = plugin_main_utils::normalize_rtmp_url_candidate(
                    obs_data_get_string(settings, keys[i]));
                if (!url.empty()) break;
            }
        }

        const char* key_keys[] = {
            "key",
            "stream_key",
            "streamkey",
            "play_path",
            "path",
            nullptr
        };
        for (int i = 0; key_keys[i]; ++i) {
            stream_key = plugin_main_utils::trim_copy(
                obs_data_get_string(settings, key_keys[i]));
            if (!stream_key.empty()) break;
        }
        obs_data_release(settings);
    }

    if (!url.empty() && !stream_key.empty()) {
        url = plugin_main_utils::join_rtmp_url_and_stream_key(url, stream_key);
    }

    if (owned && service_release) {
        service_release(owned);
    }
    return url;
}

void maybe_autofill_rtmp_url(obs_data_t* settings, bool force_refresh) {
    if (!settings) return;
    if (!obs_data_get_bool(settings, "rtmp_url_auto")) return;
    std::string configured =
        plugin_main_utils::trim_copy(obs_data_get_string(settings, "rtmp_url"));
    if (!force_refresh && !configured.empty()) return;
    std::string auto_url = get_obs_stream_url();
    if (auto_url.empty()) return;
    if (configured == auto_url) return;
    obs_data_set_string(settings, "rtmp_url", auto_url.c_str());
}

void maybe_autofill_rtmp_url_from_source(obs_source_t* source, bool force_refresh) {
    if (!source) return;
    obs_data_t* s = obs_source_get_settings(source);
    if (!s) return;
    maybe_autofill_rtmp_url(s, force_refresh);
    obs_data_release(s);
}

} // namespace plugin_main_obs_services
