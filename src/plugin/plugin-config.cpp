#include "plugin/plugin-config.hpp"

#include "core/constants.hpp"
#include "plugin/plugin-settings.hpp"
#include "plugin/plugin-utils.hpp"

#define T_(s) obs_module_text(s)

namespace ods::plugin {

	using namespace ods::core;

	std::string make_default_sub_memo(int counter) {
		const char *prefix = T_("SubDefaultMemoPrefix");
		if (!prefix || !*prefix) prefix = "Performer";
		return std::string(prefix) + make_alpha_counter_label(counter);
	}

	void apply_codec_option_visibility(obs_properties_t *props, obs_data_t *settings) {
		if (!props || !settings) return;
		int  codec             = (int)obs_data_get_int(settings, "audio_codec");
		bool is_opus           = (codec == 0);
		bool show_opus_options = is_opus;
		bool show_pcm_options  = !is_opus;
		if (auto *p = obs_properties_get(props, "audio_codec_opus_row")) {
			obs_property_set_visible(p, show_opus_options);
		}
		if (auto *p = obs_properties_get(props, "audio_codec_pcm_row")) {
			obs_property_set_visible(p, show_pcm_options);
		}
		// 横並び行で描画するため、隠し実体プロパティは常に非表示のまま維持する。
		if (auto *p = obs_properties_get(props, "opus_bitrate_kbps")) {
			obs_property_set_visible(p, false);
		}
		if (auto *p = obs_properties_get(props, "opus_sample_rate")) {
			obs_property_set_visible(p, false);
		}
		if (auto *p = obs_properties_get(props, "quantization_bits")) {
			obs_property_set_visible(p, false);
		}
		if (auto *p = obs_properties_get(props, "audio_mono")) {
			obs_property_set_visible(p, false);
		}
		if (auto *p = obs_properties_get(props, "pcm_downsample_ratio")) {
			obs_property_set_visible(p, false);
		}
	}

	void set_delay_stream_defaults(obs_data_t *settings) {
		if (!settings) return;
		obs_data_set_default_bool(settings, "delay_disable", false);
		obs_data_set_default_bool(settings, "ws_send_paused", false);
		obs_data_set_default_int(settings, "sub_ch_count", 1);
		// Ch.1 既定名を A にするため、次の自動払い出しは B から開始する。
		obs_data_set_default_int(settings, "sub_memo_auto_counter", 1);
		obs_data_set_default_int(settings, "audio_codec", 0);
		obs_data_set_default_int(settings, "opus_bitrate_kbps", 96);
		obs_data_set_default_int(settings, "opus_sample_rate", 48000);
		obs_data_set_default_int(settings, "quantization_bits", 8);
		obs_data_set_default_bool(settings, "audio_mono", true);
		obs_data_set_default_int(settings, "pcm_downsample_ratio", 4);
		obs_data_set_default_int(settings, "playback_buffer_ms", PLAYBACK_BUFFER_DEFAULT_MS);
		obs_data_set_default_int(settings, "ws_port", WS_PORT);
		obs_data_set_default_int(settings, "ping_count", DEFAULT_PING_COUNT);
		obs_data_set_default_bool(settings, "auto_measure", false);
		obs_data_set_default_bool(settings, "show_advanced", false);
		obs_data_set_default_bool(settings, "url_share_show_list", false);
		obs_data_set_default_string(settings, "stream_id", "");
		obs_data_set_default_string(settings, "host_ip_manual", "");
		obs_data_set_default_int(settings, kAvatarLatencyKey, 200);
		obs_data_set_default_int(settings, kMeasuredRtspE2eKey, 0);
		obs_data_set_default_bool(settings, kRtspE2eMeasuredKey, false);
		obs_data_set_default_int(settings, "delay_table_selected_ch", 0);
		obs_data_set_default_int(settings, "active_tab", 0);
		obs_data_set_default_bool(settings, "rtmp_url_auto", true);
		obs_data_set_default_string(settings, "rtmp_url", "");
		obs_data_set_default_bool(settings, kRtspUseRtmpUrlKey, true);
		obs_data_set_default_string(settings, kRtspUrlKey, "");
		obs_data_set_default_string(settings, kFfmpegExePathKey, "auto");
		obs_data_set_default_int(settings, kFfmpegExePathModeKey, static_cast<int>(ExePathMode::Auto));
		obs_data_set_default_string(settings, kCloudflaredExePathKey, "auto");
		obs_data_set_default_int(settings, kCloudflaredExePathModeKey, static_cast<int>(ExePathMode::Auto));
		for (int i = 0; i < MAX_SUB_CH; ++i) {
			const auto measured_key = make_sub_measured_key(i);
			obs_data_set_default_int(settings, measured_key.data(), 0);
			const auto ws_measured_key = make_sub_ws_measured_key(i);
			obs_data_set_default_bool(settings, ws_measured_key.data(), false);
			const auto offset_key = make_sub_offset_key(i);
			obs_data_set_default_int(settings, offset_key.data(), 40);
			const auto memo_key = make_sub_memo_key(i);
			if (i == 0) {
				std::string default_memo = make_default_sub_memo(0);
				obs_data_set_default_string(settings, memo_key.data(), default_memo.c_str());
			} else {
				obs_data_set_default_string(settings, memo_key.data(), "");
			}
		}
	}

} // namespace ods::plugin
