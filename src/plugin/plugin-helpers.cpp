#include "plugin/plugin-helpers.hpp"

#include <cstring>

#include "plugin/plugin-services.hpp"
#include "tunnel/tunnel-manager.hpp"

namespace plugin_main_settings_helpers {

std::string resolve_rtmp_url_from_source(obs_source_t* source) {
    if (!source) return "";
    std::string url;
    obs_data_t* s = obs_source_get_settings(source);
    if (!s) return "";
    bool auto_mode = obs_data_get_bool(s, "rtmp_url_auto");
    if (auto_mode) {
        url = plugin_main_obs_services::get_obs_stream_url();
    }
    if (url.empty()) {
        const char* configured = obs_data_get_string(s, "rtmp_url");
        if (configured && *configured) url = configured;
    }
    obs_data_release(s);
    return url;
}

void maybe_fill_cloudflared_path_from_auto(obs_source_t* source) {
    if (!source) return;
    obs_data_t* s = obs_source_get_settings(source);
    if (!s) return;
    const char* cur = obs_data_get_string(s, "cloudflared_exe_path");
    if (!cur || !*cur) {
        obs_data_set_string(s, "cloudflared_exe_path", "auto");
    }
    obs_data_release(s);
}

void maybe_persist_cloudflared_path_after_auto_ready(obs_source_t* source) {
    if (!source) return;
    std::string auto_path;
    if (!TunnelManager::get_auto_cloudflared_path_if_exists(auto_path)) return;
    std::string ui_path = TunnelManager::to_localappdata_env_path(auto_path);

    obs_data_t* s = obs_source_get_settings(source);
    if (!s) return;
    const char* cur = obs_data_get_string(s, "cloudflared_exe_path");
    bool is_auto = (!cur || !*cur || _stricmp(cur, "auto") == 0);
    if (is_auto && (!cur || _stricmp(cur, ui_path.c_str()) != 0)) {
        obs_data_set_string(s, "cloudflared_exe_path", ui_path.c_str());
    }
    obs_data_release(s);
}

} // namespace plugin_main_settings_helpers
