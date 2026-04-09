#include "plugin/plugin-settings.hpp"

#include "audio/audio-processor.hpp"
#include "core/constants.hpp"
#include "plugin/plugin-helpers.hpp"
#include "plugin/plugin-services.hpp"
#include "plugin/plugin-state.hpp"
#include "plugin/plugin-utils.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace ods::plugin {

	using namespace ods::core;

	SubSettingKey make_sub_key(const char *suffix, int ch) {
		SubSettingKey key{};
		snprintf(key.data(), key.size(), "sub%d_%s", ch, suffix);
		return key;
	}

	SubSettingKey make_sub_base_delay_key(int ch) {
		return make_sub_key("delay_ms", ch);
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

	int calc_sub_delay_raw_value_ms(int base_delay_ms,
									int offset_ms,
									int master_offset_ms,
									int master_base_delay_ms) {
		return base_delay_ms + offset_ms + master_offset_ms + master_base_delay_ms;
	}

	int calc_effective_sub_delay_value_ms(int base_delay_ms,
										  int offset_ms,
										  int master_offset_ms,
										  int master_base_delay_ms) {
		int effective = calc_sub_delay_raw_value_ms(base_delay_ms, offset_ms, master_offset_ms, master_base_delay_ms);
		if (effective < 0) effective = 0;
		return effective;
	}

	namespace {

		constexpr int kSubOffsetMinMs    = -3000;
		constexpr int kSubOffsetMaxMs    = 3000;
		constexpr int kMasterOffsetMinMs = -3000;
		constexpr int kMasterOffsetMaxMs = 3000;

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

				bool effective_delay_changed = apply_delay_settings();

				data_->rtmp_url_auto.store(obs_data_get_bool(settings_, "rtmp_url_auto"), std::memory_order_relaxed);
				apply_ping_count();

				if (effective_delay_changed) {
					data_->request_props_refresh_for_tabs({3, 4, 5}, "update.effective_changed");
				}
				data_->prev_stream_id_has_user_value = obs_data_has_user_value(settings_, "stream_id");
			}

		private:

			// UI 制御用の active_tab を正規化して保持する。
			void apply_active_tab() {
				int active_tab = static_cast<int>(obs_data_get_int(settings_, "active_tab"));
				if (active_tab < 0 || active_tab >= 6) {
					active_tab = 0;
					obs_data_set_int(settings_, "active_tab", active_tab);
				}
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
			}

			// サブチャンネル数を正規化して関連状態へ適用する。
			void apply_sub_channel_count() {
				int  current          = data_->sub_ch_count;
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
				data_->sub_ch_count = clamped;
				data_->router.set_active_channels(clamped);
				data_->flow.set_active_channels(clamped);
				if (changed) data_->flow.reset();
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

			// マスター/サブ遅延を反映し、実効値変化の有無を返す。
			bool apply_delay_settings() {
				const int prev_master_base_delay = data_->master_base_delay_ms;
				data_->master_base_delay_ms      = get_ms_int(settings_, kMasterBaseDelayKey);
				// OBS本流は遅延させない。master_base_delay_ms はサブ配信側へ加算する。
				data_->master_buf.set_delay_ms(0);

				const int    prev_master_offset = data_->master_offset_ms;
				const double raw_master_offset  = obs_data_get_double(settings_, kMasterOffsetKey);
				int          master_offset_ms   = static_cast<int>(std::lround(raw_master_offset));
				if (master_offset_ms < kMasterOffsetMinMs) {
					master_offset_ms = kMasterOffsetMinMs;
				} else if (master_offset_ms > kMasterOffsetMaxMs) {
					master_offset_ms = kMasterOffsetMaxMs;
				}
				data_->master_offset_ms = master_offset_ms;
				if (std::fabs(raw_master_offset - static_cast<double>(master_offset_ms)) > 0.001) {
					obs_data_set_int(settings_, kMasterOffsetKey, master_offset_ms);
				}

				bool effective_delay_changed = false;
				for (int i = 0; i < MAX_SUB_CH; ++i) {
					const int prev_base_delay = data_->sub_channels[i].base_delay_ms;
					const int prev_offset     = data_->sub_channels[i].offset_ms;

					const auto base_delay_key            = make_sub_base_delay_key(i);
					const int  new_base_delay_ms         = get_ms_int(settings_, base_delay_key.data());
					data_->sub_channels[i].base_delay_ms = new_base_delay_ms;

					const auto   offset_key    = make_sub_offset_key(i);
					const double raw_offset_ms = obs_data_get_double(settings_, offset_key.data());
					int          offset_ms     = static_cast<int>(std::lround(raw_offset_ms));
					if (offset_ms < kSubOffsetMinMs) {
						offset_ms = kSubOffsetMinMs;
					} else if (offset_ms > kSubOffsetMaxMs) {
						offset_ms = kSubOffsetMaxMs;
					}
					data_->sub_channels[i].offset_ms = offset_ms;
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
					ods::audio::apply_sub_delay_to_buffer(data_, i);

					const int prev_raw       = calc_sub_delay_raw_value_ms(prev_base_delay, prev_offset, prev_master_offset, prev_master_base_delay);
					const int new_raw        = calc_sub_delay_raw_value_ms(new_base_delay_ms, offset_ms, data_->master_offset_ms, data_->master_base_delay_ms);
					const int prev_effective = calc_effective_sub_delay_value_ms(prev_base_delay, prev_offset, prev_master_offset, prev_master_base_delay);
					const int new_effective  = calc_effective_sub_delay_value_ms(new_base_delay_ms, offset_ms, data_->master_offset_ms, data_->master_base_delay_ms);
					if (prev_effective != new_effective) {
						data_->router.notify_apply_delay(i, new_effective, "manual_adjust");
						effective_delay_changed = true;
					} else if (prev_raw != new_raw) {
						effective_delay_changed = true;
					}
				}

				return effective_delay_changed;
			}

			// Ping 回数設定を正規化して flow へ適用する。
			void apply_ping_count() {
				int pc = (int)obs_data_get_int(settings_, "ping_count");
				if (pc != 10 && pc != 20 && pc != 30 && pc != 40 && pc != 50) {
					pc = DEFAULT_PING_COUNT;
					obs_data_set_int(settings_, "ping_count", pc);
				}
				data_->ping_count_setting.store(pc, std::memory_order_relaxed);
				data_->flow.set_ping_count(pc);
			}
		};

	} // namespace

	void apply_settings(DelayStreamData *d, obs_data_t *settings) {
		if (!d || !settings) return;

		if (d->is_duplicate_instance) {
			d->enabled.store(false, std::memory_order_relaxed);
			d->ws_send_enabled.store(false, std::memory_order_relaxed);
			d->prev_stream_id_has_user_value = obs_data_has_user_value(settings, "stream_id");
			return;
		}

		SettingsApplier(d, settings).apply_all();
	}

} // namespace ods::plugin
