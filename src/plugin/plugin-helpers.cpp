#include "plugin/plugin-helpers.hpp"

#include "plugin/plugin-settings.hpp"
#include "plugin/plugin-services.hpp"
#include "tunnel/tunnel-manager.hpp"

#include <cstring>

namespace ods::plugin {

	bool try_get_parent_audio_sync_offset_ns(obs_source_t *filter_source, int64_t &out_offset_ns) {
		if (!filter_source) return false;
		obs_source_t *parent = obs_filter_get_parent(filter_source);
		if (!parent) parent = obs_filter_get_target(filter_source);
		if (!parent || is_obs_source_removed(parent)) return false;
		out_offset_ns = obs_source_get_sync_offset(parent);
		return true;
	}

	std::string resolve_rtmp_url_from_source(obs_source_t *source) {
		if (!source) return "";
		std::string url;
		obs_data_t *s = obs_source_get_settings(source);
		if (!s) return "";
		bool auto_mode = obs_data_get_bool(s, "rtmp_url_auto");
		if (auto_mode) {
			url = get_obs_stream_url();
		}
		if (url.empty()) {
			const char *configured = obs_data_get_string(s, "rtmp_url");
			if (configured && *configured) url = configured;
		}
		obs_data_release(s);
		return url;
	}

	std::string to_rtsp_url_from_rtmp(const std::string &rtmp_url) {
		if (rtmp_url.rfind("rtmp://", 0) == 0) {
			return "rtsp://" + rtmp_url.substr(7);
		}
		if (rtmp_url.rfind("rtmps://", 0) == 0) {
			return "rtsps://" + rtmp_url.substr(8);
		}
		return rtmp_url;
	}

	void maybe_fill_cloudflared_path_from_auto(obs_source_t *source) {
		if (!source) return;
		obs_data_t *s = obs_source_get_settings(source);
		if (!s) return;
		const char       *cur  = obs_data_get_string(s, kCloudflaredExePathKey);
		const ExePathMode mode = normalize_exe_path_mode(
			static_cast<int>(obs_data_get_int(s, kCloudflaredExePathModeKey)));
		if (mode == ExePathMode::Auto && (!cur || !*cur)) {
			obs_data_set_string(s, kCloudflaredExePathKey, "auto");
		}
		obs_data_release(s);
	}

	void maybe_persist_cloudflared_path_after_auto_ready(obs_source_t *source) {
		if (!source) return;
		std::string auto_path;
		if (!ods::tunnel::TunnelManager::get_auto_cloudflared_path_if_exists(auto_path)) return;
		std::string ui_path = ods::tunnel::TunnelManager::to_localappdata_env_path(auto_path);

		obs_data_t *s = obs_source_get_settings(source);
		if (!s) return;
		const ExePathMode mode = normalize_exe_path_mode(
			static_cast<int>(obs_data_get_int(s, kCloudflaredExePathModeKey)));
		if (mode != ExePathMode::Auto) {
			const char *cur = obs_data_get_string(s, kCloudflaredExePathKey);
			if (!cur || _stricmp(cur, ui_path.c_str()) != 0) {
				obs_data_set_string(s, kCloudflaredExePathKey, ui_path.c_str());
			}
		}
		obs_data_release(s);
	}

} // namespace ods::plugin
