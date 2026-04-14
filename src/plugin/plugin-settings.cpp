#include "plugin/plugin-settings.hpp"

#include "core/constants.hpp"
#include "plugin/plugin-helpers.hpp"
#include "plugin/plugin-services.hpp"
#include "plugin/plugin-state.hpp"
#include "plugin/plugin-utils.hpp"

#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ods::plugin {

	using namespace ods::core;

	SubSettingKey make_sub_key(const char *suffix, int ch) {
		SubSettingKey key{};
		snprintf(key.data(), key.size(), "sub%d_%s", ch, suffix);
		return key;
	}

	SubSettingKey make_sub_measured_key(int ch) {
		return make_sub_key("measured_ms", ch);
	}
	SubSettingKey make_sub_offset_key(int ch) {
		return make_sub_key("adjust_ms", ch);
	}
	SubSettingKey make_sub_memo_key(int ch) {
		return make_sub_key("memo", ch);
	}
	SubSettingKey make_sub_code_key(int ch) {
		return make_sub_key("code", ch);
	}
	SubSettingKey make_sub_ws_measured_key(int ch) {
		return make_sub_key("ws_measured", ch);
	}
	SubSettingKey make_sub_remove_row_key(int ch) {
		return make_sub_key("memo_remove_row", ch);
	}

	ExePathMode normalize_exe_path_mode(int raw_mode) {
		switch (raw_mode) {
		case static_cast<int>(ExePathMode::Auto):
			return ExePathMode::Auto;
		case static_cast<int>(ExePathMode::FromPath):
			return ExePathMode::FromPath;
		case static_cast<int>(ExePathMode::Absolute):
			return ExePathMode::Absolute;
		default:
			return ExePathMode::Auto;
		}
	}

	// DelaySnapshot を計算し、結果を各 DelayBuffer へ適用する。
	// 計測済みチャンネルのタイミング図もレシーバーへ再送信する。
	void recalc_all_delays(DelayStreamData *d) {
		if (!d) return;
		const auto &lo    = d->layout;
		const int   count = lo.count.load(std::memory_order_acquire);

		std::array<core::Slot, MAX_SUB_CH> order{};
		for (int i = 0; i < count; ++i)
			order[i] = lo.display_order[i];

		const DelaySnapshot snap = d->delay.calc_all_delays(order, count);

		for (int di = 0; di < count; ++di) {
			const core::Slot slot = order[di];
			uint32_t         ms   = (snap.channels[slot].total_ms > 0)
										? static_cast<uint32_t>(snap.channels[slot].total_ms)
										: 0;
			d->sub_channels[slot].buf.set_delay_ms(ms);
		}
		for (core::Slot s = 0; s < MAX_SUB_CH; ++s) {
			if (!lo.is_active(s))
				d->sub_channels[s].buf.set_delay_ms(0);
		}

		d->master_buf.set_delay_ms(static_cast<uint32_t>(snap.master_delay_ms));

		// タイミング図を全計測済みチャンネルの接続先へ再送信する
		if (d->router_running.load(std::memory_order_relaxed)) {
			for (int di = 0; di < count; ++di) {
				const core::Slot slot = order[di];
				if (!d->delay.channels[slot].ws_measured) continue;
				d->router.notify_timing_diagram(
					slot,
					d->delay.measured_rtsp_e2e_ms,
					d->delay.avatar_latency_ms,
					d->delay.playback_buffer_ms,
					snap.master_delay_ms,
					static_cast<float>(d->delay.channels[slot].measured_ms),
					snap.channels[slot].total_ms,
					d->delay.channels[slot].offset_ms,
					snap.channels[slot].provisional);
			}
		}
	}

	namespace {

		constexpr int kSubOffsetMinMs     = -3000;
		constexpr int kSubOffsetMaxMs     = 3000;
		constexpr int kAvatarLatencyMinMs = 0;
		constexpr int kAvatarLatencyMaxMs = 5000;

		// 既存設定（path文字列のみ）から cloudflared モードを推定する。
		ExePathMode infer_cloudflared_mode_from_path(const char *raw_path) {
			const std::string path = raw_path ? raw_path : "";
			if (path.empty() || _stricmp(path.c_str(), "auto") == 0) {
				return ExePathMode::Auto;
			}
			if (_stricmp(path.c_str(), kPathModeFromEnvPath) == 0) {
				return ExePathMode::FromPath;
			}

			std::string auto_path_abs;
			if (ods::tunnel::TunnelManager::get_auto_cloudflared_path_if_exists(auto_path_abs)) {
				const std::string auto_path_env =
					ods::tunnel::TunnelManager::to_localappdata_env_path(auto_path_abs);
				if (_stricmp(path.c_str(), auto_path_abs.c_str()) == 0 ||
					_stricmp(path.c_str(), auto_path_env.c_str()) == 0) {
					return ExePathMode::Auto;
				}
			}
			return ExePathMode::Absolute;
		}

		// 既存設定（path文字列のみ）から ffmpeg モードを推定する。
		ExePathMode infer_ffmpeg_mode_from_path(const char *raw_path) {
			const std::string path = raw_path ? raw_path : "";
			if (path.empty() || _stricmp(path.c_str(), "auto") == 0) {
				return ExePathMode::Auto;
			}
			if (_stricmp(path.c_str(), kPathModeFromEnvPath) == 0) {
				return ExePathMode::FromPath;
			}
			return ExePathMode::Absolute;
		}

		/// OBS 設定値（double/int 混在）を ms の整数値へ丸めて読み取る。
		int get_ms_int(obs_data_t *settings, const char *key) {
			if (!settings || !key) return 0;
			return static_cast<int>(std::lround(obs_data_get_double(settings, key)));
		}

		enum class JsonFieldType {
			String,
			Number,
			Bool,
			Null,
			Object,
			Array,
		};

		struct RequiredJsonField {
			std::string   key;
			JsonFieldType type;
		};

		struct SemVer {
			int major = 0;
			int minor = 0;
			int patch = 0;
		};

		bool fail_with_reason(std::string &reason, const std::string &msg) {
			reason = msg;
			return false;
		}

		void skip_ws(const std::string &json, size_t &pos) {
			while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
				++pos;
			}
		}

		bool parse_json_string_token(const std::string &json, size_t &pos, std::string &out) {
			if (pos >= json.size() || json[pos] != '"') return false;
			++pos;
			out.clear();
			while (pos < json.size()) {
				const char c = json[pos++];
				if (c == '"') return true;
				if (c == '\\') {
					if (pos >= json.size()) return false;
					const char esc = json[pos++];
					if (esc == 'u') {
						for (int i = 0; i < 4; ++i) {
							if (pos >= json.size()) return false;
							++pos;
						}
						out.push_back('?');
					} else {
						out.push_back(esc);
					}
					continue;
				}
				out.push_back(c);
			}
			return false;
		}

		bool parse_json_number_token(const std::string &json, size_t &pos) {
			const size_t start = pos;
			if (pos < json.size() && json[pos] == '-') ++pos;
			if (pos >= json.size() || !std::isdigit(static_cast<unsigned char>(json[pos]))) return false;
			if (json[pos] == '0') {
				++pos;
			} else {
				while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos])))
					++pos;
			}
			if (pos < json.size() && json[pos] == '.') {
				++pos;
				if (pos >= json.size() || !std::isdigit(static_cast<unsigned char>(json[pos]))) return false;
				while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos])))
					++pos;
			}
			if (pos < json.size() && (json[pos] == 'e' || json[pos] == 'E')) {
				++pos;
				if (pos < json.size() && (json[pos] == '+' || json[pos] == '-')) ++pos;
				if (pos >= json.size() || !std::isdigit(static_cast<unsigned char>(json[pos]))) return false;
				while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos])))
					++pos;
			}
			return pos > start;
		}

		bool parse_json_literal_token(const std::string &json, size_t &pos, const char *literal) {
			const size_t len = std::strlen(literal);
			if (pos + len > json.size()) return false;
			if (json.compare(pos, len, literal) != 0) return false;
			pos += len;
			return true;
		}

		bool parse_json_compound_token(const std::string &json,
									   size_t            &pos,
									   char               open_char,
									   char               close_char) {
			if (pos >= json.size() || json[pos] != open_char) return false;
			int depth = 0;
			while (pos < json.size()) {
				const char c = json[pos];
				if (c == '"') {
					std::string ignored;
					if (!parse_json_string_token(json, pos, ignored)) return false;
					continue;
				}
				if (c == open_char) {
					++depth;
					++pos;
					continue;
				}
				if (c == close_char) {
					--depth;
					++pos;
					if (depth == 0) return true;
					continue;
				}
				++pos;
			}
			return false;
		}

		bool parse_json_value_type(const std::string &json, size_t &pos, JsonFieldType &out_type) {
			if (pos >= json.size()) return false;
			const char c = json[pos];
			if (c == '"') {
				std::string ignored;
				if (!parse_json_string_token(json, pos, ignored)) return false;
				out_type = JsonFieldType::String;
				return true;
			}
			if (c == '{') {
				if (!parse_json_compound_token(json, pos, '{', '}')) return false;
				out_type = JsonFieldType::Object;
				return true;
			}
			if (c == '[') {
				if (!parse_json_compound_token(json, pos, '[', ']')) return false;
				out_type = JsonFieldType::Array;
				return true;
			}
			if (c == 't' || c == 'f') {
				if (c == 't') {
					if (!parse_json_literal_token(json, pos, "true")) return false;
				} else {
					if (!parse_json_literal_token(json, pos, "false")) return false;
				}
				out_type = JsonFieldType::Bool;
				return true;
			}
			if (c == 'n') {
				if (!parse_json_literal_token(json, pos, "null")) return false;
				out_type = JsonFieldType::Null;
				return true;
			}
			if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
				if (!parse_json_number_token(json, pos)) return false;
				out_type = JsonFieldType::Number;
				return true;
			}
			return false;
		}

		bool parse_top_level_json_types(const std::string                              &json,
										std::unordered_map<std::string, JsonFieldType> &out,
										std::string                                    &reason) {
			out.clear();
			size_t pos = 0;
			skip_ws(json, pos);
			if (pos >= json.size() || json[pos] != '{') {
				return fail_with_reason(reason, "settings JSON parse failed: root object not found");
			}
			++pos;

			while (true) {
				skip_ws(json, pos);
				if (pos >= json.size()) {
					return fail_with_reason(reason, "settings JSON parse failed: unexpected EOF");
				}
				if (json[pos] == '}') {
					++pos;
					break;
				}
				std::string key;
				if (!parse_json_string_token(json, pos, key)) {
					return fail_with_reason(reason, "settings JSON parse failed: key string");
				}
				skip_ws(json, pos);
				if (pos >= json.size() || json[pos] != ':') {
					return fail_with_reason(reason, "settings JSON parse failed: missing colon");
				}
				++pos;
				skip_ws(json, pos);

				JsonFieldType type{};
				if (!parse_json_value_type(json, pos, type)) {
					return fail_with_reason(reason, "settings JSON parse failed: value");
				}
				out[key] = type;

				skip_ws(json, pos);
				if (pos >= json.size()) {
					return fail_with_reason(reason, "settings JSON parse failed: unexpected EOF after value");
				}
				if (json[pos] == ',') {
					++pos;
					continue;
				}
				if (json[pos] == '}') {
					++pos;
					break;
				}
				return fail_with_reason(reason, "settings JSON parse failed: invalid delimiter");
			}

			skip_ws(json, pos);
			if (pos != json.size()) {
				return fail_with_reason(reason, "settings JSON parse failed: trailing characters");
			}
			return true;
		}

		const char *json_field_type_name(JsonFieldType type) {
			switch (type) {
			case JsonFieldType::String:
				return "string";
			case JsonFieldType::Number:
				return "number";
			case JsonFieldType::Bool:
				return "bool";
			case JsonFieldType::Null:
				return "null";
			case JsonFieldType::Object:
				return "object";
			case JsonFieldType::Array:
				return "array";
			default:
				return "unknown";
			}
		}

		void append_known_user_json_fields(std::vector<RequiredJsonField> &fields) {
			fields.push_back({kSettingsSchemaVersionKey, JsonFieldType::Number});
			fields.push_back({kSettingsSavedVersionKey, JsonFieldType::String});
			fields.push_back({"delay_disable", JsonFieldType::Bool});
			fields.push_back({"ws_send_paused", JsonFieldType::Bool});
			fields.push_back({"sub_ch_count", JsonFieldType::Number});
			fields.push_back({"sub_memo_auto_counter", JsonFieldType::Number});
			fields.push_back({"audio_codec", JsonFieldType::Number});
			fields.push_back({"opus_bitrate_kbps", JsonFieldType::Number});
			fields.push_back({"opus_sample_rate", JsonFieldType::Number});
			fields.push_back({"quantization_bits", JsonFieldType::Number});
			fields.push_back({"audio_mono", JsonFieldType::Bool});
			fields.push_back({"pcm_downsample_ratio", JsonFieldType::Number});
			fields.push_back({"playback_buffer_ms", JsonFieldType::Number});
			fields.push_back({"ws_port", JsonFieldType::Number});
			fields.push_back({"ping_count", JsonFieldType::Number});
			fields.push_back({"auto_measure", JsonFieldType::Bool});
			fields.push_back({"show_advanced", JsonFieldType::Bool});
			fields.push_back({"url_share_show_list", JsonFieldType::Bool});
			fields.push_back({"stream_id", JsonFieldType::String});
			fields.push_back({"host_ip_manual", JsonFieldType::String});
			fields.push_back({kAvatarLatencyKey, JsonFieldType::Number});
			fields.push_back({kMeasuredRtspE2eKey, JsonFieldType::Number});
			fields.push_back({kRtspE2eMeasuredKey, JsonFieldType::Bool});
			fields.push_back({"delay_table_selected_ch", JsonFieldType::Number});
			fields.push_back({"active_tab", JsonFieldType::Number});
			fields.push_back({"rtmp_url_auto", JsonFieldType::Bool});
			fields.push_back({"rtmp_url", JsonFieldType::String});
			fields.push_back({kRtspUseRtmpUrlKey, JsonFieldType::Bool});
			fields.push_back({kRtspUrlKey, JsonFieldType::String});
			fields.push_back({kFfmpegExePathKey, JsonFieldType::String});
			fields.push_back({kFfmpegExePathModeKey, JsonFieldType::Number});
			fields.push_back({kCloudflaredExePathKey, JsonFieldType::String});
			fields.push_back({kCloudflaredExePathModeKey, JsonFieldType::Number});
			for (int i = 0; i < MAX_SUB_CH; ++i) {
				fields.push_back({make_sub_measured_key(i).data(), JsonFieldType::Number});
				fields.push_back({make_sub_ws_measured_key(i).data(), JsonFieldType::Bool});
				fields.push_back({make_sub_offset_key(i).data(), JsonFieldType::Number});
				fields.push_back({make_sub_memo_key(i).data(), JsonFieldType::String});
				fields.push_back({make_sub_code_key(i).data(), JsonFieldType::String});
			}
		}

		void append_required_core_user_json_fields(std::vector<RequiredJsonField> &fields) {
			fields.push_back({kSettingsSchemaVersionKey, JsonFieldType::Number});
			fields.push_back({kSettingsSavedVersionKey, JsonFieldType::String});
			fields.push_back({"delay_disable", JsonFieldType::Bool});
			fields.push_back({"ws_send_paused", JsonFieldType::Bool});
			fields.push_back({"sub_ch_count", JsonFieldType::Number});
			fields.push_back({"audio_codec", JsonFieldType::Number});
			fields.push_back({"ws_port", JsonFieldType::Number});
			fields.push_back({"ping_count", JsonFieldType::Number});
			fields.push_back({"stream_id", JsonFieldType::String});
			fields.push_back({kAvatarLatencyKey, JsonFieldType::Number});
			fields.push_back({"active_tab", JsonFieldType::Number});
			fields.push_back({"rtmp_url_auto", JsonFieldType::Bool});
		}

		bool validate_required_json_fields(
			const std::unordered_map<std::string, JsonFieldType> &json_types,
			const std::vector<RequiredJsonField>                 &required,
			std::string                                          &reason) {
			for (const auto &field : required) {
				const auto it = json_types.find(field.key);
				if (it == json_types.end()) {
					return fail_with_reason(reason, "settings key missing: " + field.key);
				}
				if (it->second != field.type) {
					return fail_with_reason(
						reason,
						"settings type mismatch: " + field.key +
							" expected=" + json_field_type_name(field.type) +
							" actual=" + json_field_type_name(it->second));
				}
			}
			return true;
		}

		bool validate_known_json_field_types(
			const std::unordered_map<std::string, JsonFieldType> &json_types,
			std::string                                          &reason) {
			std::vector<RequiredJsonField> known;
			known.reserve(256);
			append_known_user_json_fields(known);
			for (const auto &field : known) {
				const auto it = json_types.find(field.key);
				if (it == json_types.end()) continue;
				if (it->second != field.type) {
					return fail_with_reason(
						reason,
						"settings type mismatch: " + field.key +
							" expected=" + json_field_type_name(field.type) +
							" actual=" + json_field_type_name(it->second));
				}
			}
			return true;
		}

		bool parse_unsigned_int_component(const std::string &s, size_t &pos, int &out) {
			if (pos >= s.size() || !std::isdigit(static_cast<unsigned char>(s[pos]))) return false;
			int value = 0;
			while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
				value = value * 10 + static_cast<int>(s[pos] - '0');
				++pos;
			}
			out = value;
			return true;
		}

		bool parse_semver(const std::string &raw, SemVer &out) {
			std::string s = trim_copy(raw);
			if (s.empty()) return false;
			if (s[0] == 'v' || s[0] == 'V') s.erase(s.begin());

			size_t pos = 0;
			if (!parse_unsigned_int_component(s, pos, out.major)) return false;
			if (pos >= s.size() || s[pos] != '.') return false;
			++pos;
			if (!parse_unsigned_int_component(s, pos, out.minor)) return false;

			out.patch = 0;
			if (pos < s.size() && s[pos] == '.') {
				++pos;
				if (!parse_unsigned_int_component(s, pos, out.patch)) return false;
			}

			if (pos == s.size()) return true;
			if (s[pos] == '-' || s[pos] == '+') return true;
			return false;
		}

		bool is_data_version_incompatible(const SemVer &data_ver, const SemVer &plugin_ver) {
			if (data_ver.major != plugin_ver.major) return true;
			if (data_ver.minor > plugin_ver.minor) return true;
			return false;
		}

		bool is_valid_ping_count(int pc) {
			return pc == 10 || pc == 20 || pc == 30 || pc == 40 || pc == 50;
		}

		bool is_valid_path_mode_raw(int mode) {
			return mode == static_cast<int>(ExePathMode::Auto) ||
				   mode == static_cast<int>(ExePathMode::FromPath) ||
				   mode == static_cast<int>(ExePathMode::Absolute);
		}

		// update の設定反映を段階化し、項目間の依存をまとめて扱う。
		class SettingsApplier {
			DelayStreamData *data_;
			obs_data_t      *settings_;

		public:

			SettingsApplier(DelayStreamData *d, obs_data_t *s) : data_(d), settings_(s) {}

			// update 入力を一括反映するエントリーポイント。
			void apply_all() {
				bool stream_id_has_user_value = obs_data_has_user_value(settings_, "stream_id");
				bool reset_to_defaults =
					data_->create_done.load(std::memory_order_relaxed) &&
					data_->prev_stream_id_has_user_value &&
					!stream_id_has_user_value;

				if (reset_to_defaults) handle_defaults_reset();

				apply_active_tab();
				apply_basic_flags();
				apply_sub_channel_count();
				apply_audio_codec_settings();
				apply_stream_endpoint_settings();

				apply_delay_settings();

				data_->rtmp_url_auto.store(obs_data_get_bool(settings_, "rtmp_url_auto"), std::memory_order_relaxed);
				apply_ping_count();
				persist_settings_snapshot();
				data_->prev_stream_id_has_user_value = obs_data_has_user_value(settings_, "stream_id");
			}

		private:

			// UI 制御用の active_tab を正規化して保持する。
			// create 完了前（OBS 再起動直後の初回ロード）は保存値を無視し、
			// 出演者名タブをアクティブにする。
			void apply_active_tab() {
				int active_tab;
				if (!data_->create_done.load(std::memory_order_relaxed)) {
					active_tab = TAB_PERFORMER_NAMES;
				} else {
					active_tab = static_cast<int>(obs_data_get_int(settings_, "active_tab"));
					if (active_tab < 0 || active_tab >= TAB_COUNT)
						active_tab = TAB_PERFORMER_NAMES;
				}
				obs_data_set_int(settings_, "active_tab", active_tab);
				data_->set_active_tab(active_tab);
			}

			// 設定リセット時に稼働中の通信系コンポーネントを停止する。
			void handle_defaults_reset() {
				if (data_->router_running.load(std::memory_order_relaxed)) {
					data_->router.stop();
					data_->router_running.store(false, std::memory_order_relaxed);
					blog(LOG_INFO, "[obs-delay-stream] WebSocket server stopped by defaults reset");
				}
				ods::tunnel::TunnelState ts = data_->tunnel.state();
				if (ts == TunnelState::Starting || ts == TunnelState::Running) {
					data_->tunnel.stop();
					blog(LOG_INFO, "[obs-delay-stream] Tunnel stopped by defaults reset");
				}
			}

			// 有効/一時停止フラグを反映する。
			void apply_basic_flags() {
				bool delay_disable = obs_data_get_bool(settings_, "delay_disable");
				data_->enabled.store(!delay_disable);
				bool paused = obs_data_get_bool(settings_, "ws_send_paused");
				data_->ws_send_enabled.store(!paused);
				bool new_auto = obs_data_get_bool(settings_, "auto_measure");
				bool old_auto = data_->auto_measure_enabled.exchange(new_auto);
				// OFF→ON 切り替え時: 接続済みかつ未計測のチャンネルを即座にスキャン
				if (new_auto && !old_auto && data_->trigger_auto_measure_scan)
					data_->trigger_auto_measure_scan();
			}

			// 表示順テーブルを復元してルーターのアクティブスロットへ反映する。
			void apply_sub_channel_count() {
				int  current          = data_->layout.count.load(std::memory_order_relaxed);
				bool has_sub_ch_count = obs_data_has_user_value(settings_, "sub_ch_count");
				int  raw_v            = has_sub_ch_count ? (int)obs_data_get_int(settings_, "sub_ch_count") : current;
				int  v                = raw_v;
				if (v <= 0) v = (current > 0) ? current : 1;
				int clamped = clamp_sub_ch_count(v);
				if (!has_sub_ch_count || clamped != v) {
					obs_data_set_int(settings_, "sub_ch_count", clamped);
				}
				bool changed = (clamped != current);
				if (changed || !has_sub_ch_count || raw_v != clamped) {
					blog(LOG_INFO,
						 "[obs-delay-stream] update sub_ch_count current=%d has_user=%d raw=%d normalized=%d clamped=%d",
						 current,
						 has_sub_ch_count ? 1 : 0,
						 raw_v,
						 v,
						 clamped);
				}

				// 表示順テーブルを復元する。キー未設定の場合は 0..sub_ch_count-1
				const char *order_raw = obs_data_get_string(settings_, kChDisplayOrderKey);
				std::string order_str = order_raw ? order_raw : "";
				if (order_str.empty()) {
					data_->layout.display_order.fill(-1);
					for (int i = 0; i < clamped; ++i)
						data_->layout.display_order[i] = i;
					data_->layout.count.store(clamped, std::memory_order_relaxed);
				} else {
					data_->layout.deserialize(order_str);
				}
				// ルーターのアクティブスロットを layout と同期
				const int n = data_->layout.count.load(std::memory_order_relaxed);
				for (int i = 0; i < n; ++i)
					data_->router.activate_slot(data_->layout.display_order[i]);
			}

			// 音声コーデック関連設定を正規化して router へ適用する。
			void apply_audio_codec_settings() {
				AudioConfig audio_cfg{};
				audio_cfg.codec = (int)obs_data_get_int(settings_, "audio_codec");
				{
					int bitrate = (int)obs_data_get_int(settings_, "opus_bitrate_kbps");
					if (bitrate < 24) {
						bitrate = 24;
						obs_data_set_int(settings_, "opus_bitrate_kbps", bitrate);
					} else if (bitrate > 320) {
						bitrate = 320;
						obs_data_set_int(settings_, "opus_bitrate_kbps", bitrate);
					}
					audio_cfg.opus_bitrate_kbps = bitrate;
				}
				{
					int raw         = (int)obs_data_get_int(settings_, "opus_sample_rate");
					int sample_rate = normalize_opus_sample_rate(raw);
					if (sample_rate != raw) obs_data_set_int(settings_, "opus_sample_rate", sample_rate);
					audio_cfg.opus_target_sample_rate = sample_rate;
				}
				{
					int raw        = (int)obs_data_get_int(settings_, "quantization_bits");
					int quant_bits = normalize_quantization_bits(raw);
					if (quant_bits != raw) obs_data_set_int(settings_, "quantization_bits", quant_bits);
					audio_cfg.quantization_bits = quant_bits;
				}
				audio_cfg.mono = obs_data_get_bool(settings_, "audio_mono");
				{
					int raw   = (int)obs_data_get_int(settings_, "pcm_downsample_ratio");
					int ratio = normalize_pcm_downsample_ratio(raw);
					if (ratio != raw) obs_data_set_int(settings_, "pcm_downsample_ratio", ratio);
					audio_cfg.pcm_downsample_ratio = ratio;
				}
				{
					int raw_pb_ms = (int)obs_data_get_int(settings_, "playback_buffer_ms");
					int pb_ms     = normalize_playback_buffer_ms(raw_pb_ms);
					if (pb_ms != raw_pb_ms) {
						obs_data_set_int(settings_, "playback_buffer_ms", pb_ms);
					}
					data_->playback_buffer_ms    = pb_ms;
					audio_cfg.playback_buffer_ms = pb_ms;
				}
				data_->router.set_audio_config(audio_cfg);
			}

			// 配信先識別子や接続先情報を更新する。
			void apply_stream_endpoint_settings() {
				const char *raw = obs_data_get_string(settings_, "stream_id");
				std::string sid = raw ? sanitize_stream_id(raw) : "";
				if (sid.empty()) {
					if (!data_->sid_autofill_guard.exchange(true)) {
						sid = generate_stream_id(12);
						obs_data_set_string(settings_, "stream_id", sid.c_str());
						data_->sid_autofill_guard.store(false);
					} else {
						sid = raw ? sanitize_stream_id(raw) : "";
					}
				}
				data_->set_stream_id(sid);
				data_->router.set_stream_id(sid);
				maybe_autofill_rtmp_url(settings_, true);
				if (obs_data_get_bool(settings_, kRtspUseRtmpUrlKey)) {
					const char *raw_rtmp = obs_data_get_string(settings_, "rtmp_url");
					std::string rtmp_url = raw_rtmp ? raw_rtmp : "";
					std::string rtsp_url = to_rtsp_url_from_rtmp(rtmp_url);
					if (!rtsp_url.empty()) {
						obs_data_set_string(settings_, kRtspUrlKey, rtsp_url.c_str());
					}
				}
				maybe_fill_cloudflared_path_from_auto(data_->context);

				const char       *cloudflared_path     = obs_data_get_string(settings_, kCloudflaredExePathKey);
				const int         cloudflared_mode_raw = obs_data_has_user_value(settings_, kCloudflaredExePathModeKey)
															 ? static_cast<int>(obs_data_get_int(settings_, kCloudflaredExePathModeKey))
															 : static_cast<int>(infer_cloudflared_mode_from_path(cloudflared_path));
				const ExePathMode cloudflared_mode     = normalize_exe_path_mode(cloudflared_mode_raw);
				obs_data_set_int(settings_, kCloudflaredExePathModeKey, static_cast<int>(cloudflared_mode));

				const char       *ffmpeg_path     = obs_data_get_string(settings_, kFfmpegExePathKey);
				const int         ffmpeg_mode_raw = obs_data_has_user_value(settings_, kFfmpegExePathModeKey)
														? static_cast<int>(obs_data_get_int(settings_, kFfmpegExePathModeKey))
														: static_cast<int>(infer_ffmpeg_mode_from_path(ffmpeg_path));
				const ExePathMode ffmpeg_mode     = normalize_exe_path_mode(ffmpeg_mode_raw);
				obs_data_set_int(settings_, kFfmpegExePathModeKey, static_cast<int>(ffmpeg_mode));

				{
					int ws_port = (int)obs_data_get_int(settings_, "ws_port");
					if (ws_port < 1 || ws_port > 65535) {
						ws_port = WS_PORT;
						obs_data_set_int(settings_, "ws_port", ws_port);
					}
					data_->ws_port.store(ws_port, std::memory_order_relaxed);
				}

				const char *hip = obs_data_get_string(settings_, "host_ip_manual");
				data_->set_host_ip(hip);
			}

			// アバター遅延・計測結果・オフセットを反映し、全チャンネルディレイを再計算する。
			void apply_delay_settings() {
				auto &delay = data_->delay;

				// アバターレイテンシ
				{
					int avatar = static_cast<int>(obs_data_get_int(settings_, kAvatarLatencyKey));
					if (avatar < kAvatarLatencyMinMs) avatar = kAvatarLatencyMinMs;
					if (avatar > kAvatarLatencyMaxMs) avatar = kAvatarLatencyMaxMs;
					delay.avatar_latency_ms = avatar;
				}

				// 再生バッファ
				delay.playback_buffer_ms = data_->playback_buffer_ms;

				// RTSP E2E 計測結果（OBS 設定から復元）
				delay.measured_rtsp_e2e_ms =
					static_cast<int>(obs_data_get_int(settings_, kMeasuredRtspE2eKey));
				delay.rtsp_e2e_measured = obs_data_get_bool(settings_, kRtspE2eMeasuredKey);

				for (int i = 0; i < MAX_SUB_CH; ++i) {
					auto &ch = delay.channels[i];

					// チャンネル計測結果（OBS 設定から復元）
					const auto measured_key = make_sub_measured_key(i);
					ch.measured_ms =
						static_cast<int>(obs_data_get_int(settings_, measured_key.data()));
					const auto ws_measured_key = make_sub_ws_measured_key(i);
					ch.ws_measured =
						obs_data_get_bool(settings_, ws_measured_key.data());

					// チャンネル別オフセット
					const auto   offset_key    = make_sub_offset_key(i);
					const double raw_offset_ms = obs_data_get_double(settings_, offset_key.data());
					int          offset_ms     = static_cast<int>(std::lround(raw_offset_ms));
					if (offset_ms < kSubOffsetMinMs) offset_ms = kSubOffsetMinMs;
					if (offset_ms > kSubOffsetMaxMs) offset_ms = kSubOffsetMaxMs;
					// 負のオフセットがブラウザ配信レイテンシを超えないよう制限
					if (ch.ws_measured && offset_ms < -ch.measured_ms)
						offset_ms = -ch.measured_ms;
					ch.offset_ms = offset_ms;
					if (std::fabs(raw_offset_ms - static_cast<double>(offset_ms)) > 0.001) {
						obs_data_set_int(settings_, offset_key.data(), offset_ms);
					}

					const auto  memo_key = make_sub_memo_key(i);
					const char *memo     = obs_data_get_string(settings_, memo_key.data());
					data_->router.set_sub_memo(i, memo ? memo : "");

					const auto  code_key = make_sub_code_key(i);
					const char *code_raw = obs_data_get_string(settings_, code_key.data());
					std::string code     = code_raw ? code_raw : "";
					if (code.empty()) {
						code = generate_stream_id(8);
						obs_data_set_string(settings_, code_key.data(), code.c_str());
					}
					data_->router.set_sub_code(i, code);
				}

				// 全チャンネル + master_buf のディレイを一括再計算する。
				recalc_all_delays(data_);

				// タイミング図・サマリテーブルを最新の計算結果で再描画する。
				data_->request_props_refresh_for_tabs({TAB_FINE_ADJUST}, "delay_recalc");
			}

			// Ping 回数設定を正規化して flow へ適用する。
			void apply_ping_count() {
				int pc = (int)obs_data_get_int(settings_, "ping_count");
				if (pc != 10 && pc != 20 && pc != 30 && pc != 40 && pc != 50) {
					pc = DEFAULT_PING_COUNT;
					obs_data_set_int(settings_, "ping_count", pc);
				}
				data_->ping_count_setting.store(pc, std::memory_order_relaxed);
			}

			// 検証対象の必須キーを同型で保存し直し、次回起動時の整合性判定に使う。
			void persist_settings_snapshot() {
				obs_data_set_int(settings_, kSettingsSchemaVersionKey, kSettingsSchemaVersion);
				obs_data_set_string(settings_, kSettingsSavedVersionKey, PLUGIN_VERSION);

				obs_data_set_bool(settings_, "delay_disable", !data_->enabled.load(std::memory_order_relaxed));
				obs_data_set_bool(settings_, "ws_send_paused", !data_->ws_send_enabled.load(std::memory_order_relaxed));
				obs_data_set_int(settings_, "sub_ch_count", data_->layout.count.load(std::memory_order_relaxed));
				obs_data_set_string(settings_, kChDisplayOrderKey, data_->layout.serialize().c_str());
				obs_data_set_int(settings_, "sub_memo_auto_counter", static_cast<int>(obs_data_get_int(settings_, "sub_memo_auto_counter")));

				obs_data_set_int(settings_, "audio_codec", static_cast<int>(obs_data_get_int(settings_, "audio_codec")));
				obs_data_set_int(settings_, "opus_bitrate_kbps", static_cast<int>(obs_data_get_int(settings_, "opus_bitrate_kbps")));
				obs_data_set_int(settings_, "opus_sample_rate", static_cast<int>(obs_data_get_int(settings_, "opus_sample_rate")));
				obs_data_set_int(settings_, "quantization_bits", static_cast<int>(obs_data_get_int(settings_, "quantization_bits")));
				obs_data_set_bool(settings_, "audio_mono", obs_data_get_bool(settings_, "audio_mono"));
				obs_data_set_int(settings_, "pcm_downsample_ratio", static_cast<int>(obs_data_get_int(settings_, "pcm_downsample_ratio")));
				obs_data_set_int(settings_, "playback_buffer_ms", data_->playback_buffer_ms);

				obs_data_set_int(settings_, "ws_port", data_->ws_port.load(std::memory_order_relaxed));
				obs_data_set_int(settings_, "ping_count", data_->ping_count_setting.load(std::memory_order_relaxed));
				obs_data_set_bool(settings_, "auto_measure", data_->auto_measure_enabled.load(std::memory_order_relaxed));
				obs_data_set_bool(settings_, "show_advanced", obs_data_get_bool(settings_, "show_advanced"));
				obs_data_set_bool(settings_, "url_share_show_list", obs_data_get_bool(settings_, "url_share_show_list"));

				const std::string sid = data_->get_stream_id();
				obs_data_set_string(settings_, "stream_id", sid.c_str());
				obs_data_set_string(settings_, "host_ip_manual", obs_data_get_string(settings_, "host_ip_manual"));

				obs_data_set_int(settings_, kAvatarLatencyKey, data_->delay.avatar_latency_ms);
				obs_data_set_int(settings_, kMeasuredRtspE2eKey, data_->delay.measured_rtsp_e2e_ms);
				obs_data_set_bool(settings_, kRtspE2eMeasuredKey, data_->delay.rtsp_e2e_measured);
				obs_data_set_int(settings_, "delay_table_selected_ch", static_cast<int>(obs_data_get_int(settings_, "delay_table_selected_ch")));
				obs_data_set_int(settings_, "active_tab", data_->get_active_tab());
				obs_data_set_bool(settings_, "rtmp_url_auto", data_->rtmp_url_auto.load(std::memory_order_relaxed));
				obs_data_set_string(settings_, "rtmp_url", obs_data_get_string(settings_, "rtmp_url"));
				obs_data_set_bool(settings_, kRtspUseRtmpUrlKey, obs_data_get_bool(settings_, kRtspUseRtmpUrlKey));
				obs_data_set_string(settings_, kRtspUrlKey, obs_data_get_string(settings_, kRtspUrlKey));
				obs_data_set_string(settings_, kFfmpegExePathKey, obs_data_get_string(settings_, kFfmpegExePathKey));
				obs_data_set_int(settings_, kFfmpegExePathModeKey, static_cast<int>(obs_data_get_int(settings_, kFfmpegExePathModeKey)));
				obs_data_set_string(settings_, kCloudflaredExePathKey, obs_data_get_string(settings_, kCloudflaredExePathKey));
				obs_data_set_int(settings_, kCloudflaredExePathModeKey, static_cast<int>(obs_data_get_int(settings_, kCloudflaredExePathModeKey)));

				for (int i = 0; i < MAX_SUB_CH; ++i) {
					const auto measured_key = make_sub_measured_key(i);
					obs_data_set_int(settings_, measured_key.data(), data_->delay.channels[i].measured_ms);

					const auto ws_measured_key = make_sub_ws_measured_key(i);
					obs_data_set_bool(settings_, ws_measured_key.data(), data_->delay.channels[i].ws_measured);

					const auto offset_key = make_sub_offset_key(i);
					obs_data_set_int(settings_, offset_key.data(), data_->delay.channels[i].offset_ms);

					const auto memo_key = make_sub_memo_key(i);
					obs_data_set_string(settings_, memo_key.data(), obs_data_get_string(settings_, memo_key.data()));

					const auto code_key = make_sub_code_key(i);
					obs_data_set_string(settings_, code_key.data(), obs_data_get_string(settings_, code_key.data()));
				}
			}
		};

	} // namespace

	bool validate_settings_compatibility(obs_data_t *settings, std::string &reason) {
		reason.clear();
		if (!settings) return fail_with_reason(reason, "settings is null");

		const char *json_raw = obs_data_get_json(settings);
		if (!json_raw) return fail_with_reason(reason, "obs_data_get_json failed");
		const std::string json(json_raw);

		std::unordered_map<std::string, JsonFieldType> json_types;
		if (!parse_top_level_json_types(json, json_types, reason)) return false;

		// 新規作成直後（未保存）だけは検証をスキップして初期保存を許可する。
		if (json_types.empty()) return true;

		std::vector<RequiredJsonField> required_core;
		required_core.reserve(32);
		append_required_core_user_json_fields(required_core);
		if (!validate_required_json_fields(json_types, required_core, reason)) return false;
		if (!validate_known_json_field_types(json_types, reason)) return false;

		const int schema_version = static_cast<int>(obs_data_get_int(settings, kSettingsSchemaVersionKey));
		if (schema_version != kSettingsSchemaVersion) {
			return fail_with_reason(
				reason,
				"settings schema version mismatch: saved=" + std::to_string(schema_version) +
					" current=" + std::to_string(kSettingsSchemaVersion));
		}

		const std::string saved_version = trim_copy(obs_data_get_string(settings, kSettingsSavedVersionKey));
		if (saved_version.empty()) {
			return fail_with_reason(reason, "settings saved version is empty");
		}
		SemVer data_ver{};
		SemVer plugin_ver{};
		if (!parse_semver(saved_version, data_ver)) {
			return fail_with_reason(reason, "settings saved version parse failed: " + saved_version);
		}
		if (!parse_semver(PLUGIN_VERSION, plugin_ver)) {
			return fail_with_reason(reason, "plugin version parse failed: " + std::string(PLUGIN_VERSION));
		}
		// 互換条件: major 一致かつ data.minor <= plugin.minor
		if (is_data_version_incompatible(data_ver, plugin_ver)) {
			return fail_with_reason(
				reason,
				"settings saved version incompatible: saved=" + saved_version +
					" current=" + std::string(PLUGIN_VERSION));
		}

		const auto has_key = [&json_types](const std::string &key) {
			return json_types.find(key) != json_types.end();
		};

		const int sub_ch_count = static_cast<int>(obs_data_get_int(settings, "sub_ch_count"));
		if (sub_ch_count < 1 || sub_ch_count > MAX_SUB_CH) {
			return fail_with_reason(reason, "sub_ch_count out of range");
		}

		const int active_tab = static_cast<int>(obs_data_get_int(settings, "active_tab"));
		if (active_tab < 0 || active_tab >= TAB_COUNT) {
			return fail_with_reason(reason, "active_tab out of range");
		}

		const int ws_port = static_cast<int>(obs_data_get_int(settings, "ws_port"));
		if (ws_port < 1 || ws_port > 65535) {
			return fail_with_reason(reason, "ws_port out of range");
		}

		const int ping_count = static_cast<int>(obs_data_get_int(settings, "ping_count"));
		if (!is_valid_ping_count(ping_count)) {
			return fail_with_reason(reason, "ping_count out of range");
		}

		if (has_key("opus_bitrate_kbps")) {
			const int bitrate = static_cast<int>(obs_data_get_int(settings, "opus_bitrate_kbps"));
			if (bitrate < 24 || bitrate > 320) {
				return fail_with_reason(reason, "opus_bitrate_kbps out of range");
			}
		}
		if (has_key("opus_sample_rate")) {
			const int sample_rate = static_cast<int>(obs_data_get_int(settings, "opus_sample_rate"));
			if (normalize_opus_sample_rate(sample_rate) != sample_rate) {
				return fail_with_reason(reason, "opus_sample_rate is invalid");
			}
		}
		if (has_key("quantization_bits")) {
			const int quant_bits = static_cast<int>(obs_data_get_int(settings, "quantization_bits"));
			if (normalize_quantization_bits(quant_bits) != quant_bits) {
				return fail_with_reason(reason, "quantization_bits is invalid");
			}
		}
		if (has_key("pcm_downsample_ratio")) {
			const int downsample_ratio = static_cast<int>(obs_data_get_int(settings, "pcm_downsample_ratio"));
			if (normalize_pcm_downsample_ratio(downsample_ratio) != downsample_ratio) {
				return fail_with_reason(reason, "pcm_downsample_ratio is invalid");
			}
		}
		if (has_key("playback_buffer_ms")) {
			const int playback_buffer_ms = static_cast<int>(obs_data_get_int(settings, "playback_buffer_ms"));
			if (normalize_playback_buffer_ms(playback_buffer_ms) != playback_buffer_ms) {
				return fail_with_reason(reason, "playback_buffer_ms out of range");
			}
		}

		const int avatar_latency = static_cast<int>(obs_data_get_int(settings, kAvatarLatencyKey));
		if (avatar_latency < kAvatarLatencyMinMs || avatar_latency > kAvatarLatencyMaxMs) {
			return fail_with_reason(reason, "avatar_latency_ms out of range");
		}

		const std::string stream_id = obs_data_get_string(settings, "stream_id");
		if (stream_id.empty()) {
			return fail_with_reason(reason, "stream_id is empty");
		}
		if (sanitize_stream_id(stream_id.c_str()) != stream_id) {
			return fail_with_reason(reason, "stream_id contains unsupported characters");
		}

		if (has_key(kFfmpegExePathModeKey)) {
			const int ffmpeg_mode_raw = static_cast<int>(obs_data_get_int(settings, kFfmpegExePathModeKey));
			if (!is_valid_path_mode_raw(ffmpeg_mode_raw)) {
				return fail_with_reason(reason, "ffmpeg_exe_path_mode is invalid");
			}
		}
		if (has_key(kCloudflaredExePathModeKey)) {
			const int cloudflared_mode_raw = static_cast<int>(obs_data_get_int(settings, kCloudflaredExePathModeKey));
			if (!is_valid_path_mode_raw(cloudflared_mode_raw)) {
				return fail_with_reason(reason, "cloudflared_exe_path_mode is invalid");
			}
		}

		for (int i = 0; i < MAX_SUB_CH; ++i) {
			const auto offset_key = make_sub_offset_key(i);

			if (has_key(offset_key.data())) {
				const int offset = get_ms_int(settings, offset_key.data());
				if (offset < kSubOffsetMinMs || offset > kSubOffsetMaxMs) {
					return fail_with_reason(reason, "sub adjust out of range: ch=" + std::to_string(i));
				}
			}
		}

		return true;
	}

	void apply_settings(DelayStreamData *d, obs_data_t *settings) {
		if (!d || !settings) return;

		if (d->is_warning_only_instance()) {
			d->enabled.store(false, std::memory_order_relaxed);
			d->ws_send_enabled.store(false, std::memory_order_relaxed);
			d->prev_stream_id_has_user_value = obs_data_has_user_value(settings, "stream_id");
			return;
		}

		SettingsApplier(d, settings).apply_all();
	}

} // namespace ods::plugin
