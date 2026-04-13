#include "ui/properties-builder.hpp"

#include "core/string-format.hpp"
#include "core/constants.hpp"
#include "model/settings-repo.hpp"
#include "plugin/plugin-config.hpp"
#include "plugin/plugin-helpers.hpp"
#include "plugin/plugin-services.hpp"
#include "plugin/plugin-settings.hpp"
#include "plugin/plugin-state.hpp"
#include "plugin/plugin-utils.hpp"
#include "ui/props-refresh.hpp"
#include "widgets/color-buttons-widget.hpp"
#include "widgets/delay-table-widget.hpp"
#include "widgets/flow-progress-widget.hpp"
#include "widgets/help-callout-widget.hpp"
#include "widgets/flow-table-widget.hpp"
#include "widgets/mode-text-row-widget.hpp"
#include "widgets/path-mode-row-widget.hpp"
#include "widgets/pulldown-row-widget.hpp"
#include "widgets/stepper-widget.hpp"
#include "widgets/button-bar-widget.hpp"
#include "widgets/text-button-widget.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#define T_(s) obs_module_text(s)

namespace ods::ui {

	using ods::plugin::DelayStreamData;
	using ods::plugin::TabCtx;
	using ods::plugin::UpdateCheckStatus;
	using ods::tunnel::TunnelState;
	using namespace ods::core;
	using namespace ods::widgets;

	namespace {
		using ods::plugin::extract_host_from_url;
		using ods::plugin::make_sub_memo_key;
		using ods::plugin::to_rtsp_url_from_rtmp;

		static constexpr int64_t REQUIRED_AUDIO_SYNC_OFFSET_NS = -950LL * 1000000LL;
		static constexpr char    kEmptyAbsolutePathSentinel[]  = "__OBS_DELAY_STREAM_EMPTY_ABSOLUTE_PATH__";

		static constexpr const char *kWarningTextColorLight = ods::core::UI_COLOR_WARNING_LIGHT;
		static constexpr const char *kWarningTextColorDark  = ods::core::UI_COLOR_WARNING_DARK;

		// ============================================================
		// namespace ローカル補助関数
		// ============================================================

		// 設定に基づいて RTMP URL から RTSP URL を同期する。
		void sync_rtsp_url_from_rtmp_if_needed(obs_data_t *settings) {
			if (!settings) return;
			if (!obs_data_get_bool(settings, ods::plugin::kRtspUseRtmpUrlKey)) return;
			const char *raw_rtmp = obs_data_get_string(settings, "rtmp_url");
			std::string rtmp_url = raw_rtmp ? raw_rtmp : "";
			std::string rtsp_url = to_rtsp_url_from_rtmp(rtmp_url);
			if (!rtsp_url.empty()) {
				obs_data_set_string(settings, ods::plugin::kRtspUrlKey, rtsp_url.c_str());
			}
		}

		// RTSP URL 入力欄の有効/無効を同期する。
		void sync_rtsp_url_enabled(obs_properties_t *props, obs_data_t *settings) {
			if (!props || !settings) return;
			const bool use_rtmp = obs_data_get_bool(settings, ods::plugin::kRtspUseRtmpUrlKey);
			if (auto *rtsp_p = obs_properties_get(props, ods::plugin::kRtspUrlKey)) {
				obs_property_set_enabled(rtsp_p, !use_rtmp);
			}
		}

		// モード設定と入力値から実際に解決へ渡すパスヒント文字列を生成する。
		std::string build_exe_path_hint(obs_data_t *settings,
										const char *mode_key,
										const char *path_key) {
			if (!settings || !mode_key || !path_key) return "auto";
			const auto mode = ods::plugin::normalize_exe_path_mode(
				static_cast<int>(obs_data_get_int(settings, mode_key)));
			if (mode == ods::plugin::ExePathMode::Auto) {
				return "auto";
			}
			if (mode == ods::plugin::ExePathMode::FromPath) {
				return ods::plugin::kPathModeFromEnvPath;
			}
			const char *raw = obs_data_get_string(settings, path_key);
			if (!raw || !*raw) return kEmptyAbsolutePathSentinel;
			if (_stricmp(raw, "auto") == 0 || _stricmp(raw, ods::plugin::kPathModeFromEnvPath) == 0) {
				return kEmptyAbsolutePathSentinel;
			}
			return raw;
		}

		// ============================================================
		// static コールバック（properties UI）
		// ============================================================

		// RTMP URL 自動設定トグルに合わせて入力有効状態を切り替える。
		bool cb_rtmp_url_auto_changed(void *priv, obs_properties_t *props, obs_property_t *, obs_data_t *settings) {
			auto *d = static_cast<DelayStreamData *>(priv);
			if (!d || !settings) return false;
			bool auto_new = obs_data_get_bool(settings, "rtmp_url_auto");
			d->rtmp_url_auto.store(auto_new, std::memory_order_relaxed);
			if (auto_new) {
				ods::plugin::maybe_autofill_rtmp_url(settings, true);
			}
			sync_rtsp_url_from_rtmp_if_needed(settings);
			if (props) {
				if (auto *url_p = obs_properties_get(props, "rtmp_url")) {
					obs_property_set_enabled(url_p, !auto_new);
				}
				sync_rtsp_url_enabled(props, settings);
			}
			return false;
		}

		// RTSP URL を RTMP URL から自動生成するかどうかを切り替える。
		bool cb_rtsp_use_rtmp_changed(void *priv, obs_properties_t *props, obs_property_t *, obs_data_t *settings) {
			(void)priv;
			if (!settings) return false;
			if (obs_data_get_bool(settings, "rtmp_url_auto")) {
				ods::plugin::maybe_autofill_rtmp_url(settings, true);
			}
			sync_rtsp_url_from_rtmp_if_needed(settings);
			sync_rtsp_url_enabled(props, settings);
			return false;
		}

		// RTMP URL 編集時に、必要なら RTSP URL へ自動同期する。
		bool cb_rtmp_url_changed(void *priv, obs_properties_t *, obs_property_t *, obs_data_t *settings) {
			(void)priv;
			if (!settings) return false;
			const bool use_rtmp_for_rtsp = obs_data_get_bool(settings, ods::plugin::kRtspUseRtmpUrlKey);
			sync_rtsp_url_from_rtmp_if_needed(settings);
			if (!use_rtmp_for_rtsp) return false;
			return false;
		}

		// WebSocket サーバーを起動し、状態表示を更新する。
		bool cb_ws_server_start(obs_properties_t *, obs_property_t *, void *priv) {
			auto *d = static_cast<DelayStreamData *>(priv);
			if (!d || d->router_running.load()) return false;
			int ws_port = d->ws_port.load(std::memory_order_relaxed);
			if (d->router.start((uint16_t)ws_port)) {
				d->router_running.store(true);
				blog(LOG_INFO, "[obs-delay-stream] WebSocket server started on port %d", ws_port);
			} else {
				blog(LOG_ERROR, "[obs-delay-stream] WebSocket server FAILED to start on port %d", ws_port);
			}
			d->request_props_refresh_for_tabs({TAB_AUDIO_STREAMING, TAB_URL_SHARING}, "cb_ws_server_start");
			return false;
		}

		// WebSocket サーバーを起動してから次のタブへ移動する。
		bool cb_ws_start_and_goto_tab(obs_properties_t *, obs_property_t *, void *priv) {
			auto *ctx = static_cast<TabCtx *>(priv);
			if (!ctx || !ctx->d || !ctx->d->context) return false;
			auto *d = ctx->d;
			// 未起動なら起動する。
			if (!d->router_running.load()) {
				int ws_port = d->ws_port.load(std::memory_order_relaxed);
				if (d->router.start(static_cast<uint16_t>(ws_port))) {
					d->router_running.store(true);
					blog(LOG_INFO, "[obs-delay-stream] WebSocket server started on port %d (start+next)", ws_port);
				} else {
					blog(LOG_ERROR, "[obs-delay-stream] WebSocket server FAILED to start on port %d", ws_port);
				}
			}
			// タブ移動
			d->set_active_tab(ctx->tab);
			obs_data_t *s = obs_source_get_settings(d->context);
			if (s) {
				obs_data_set_int(s, "active_tab", ctx->tab);
				obs_data_release(s);
			}
			return true;
		}

		// トンネルを起動してから次のタブへ移動する。
		bool cb_tunnel_start_and_goto_tab(obs_properties_t *, obs_property_t *, void *priv) {
			auto *ctx = static_cast<TabCtx *>(priv);
			if (!ctx || !ctx->d || !ctx->d->context) return false;
			auto       *d  = ctx->d;
			TunnelState ts = d->tunnel.state();
			if (ts != TunnelState::Running && ts != TunnelState::Starting) {
				obs_data_t *s = obs_source_get_settings(d->context);
				if (s) {
					const std::string exe = build_exe_path_hint(
						s,
						ods::plugin::kCloudflaredExePathModeKey,
						ods::plugin::kCloudflaredExePathKey);
					obs_data_release(s);
					int ws_port = d->ws_port.load(std::memory_order_relaxed);
					d->tunnel.start(exe, ws_port);
					blog(LOG_INFO, "[obs-delay-stream] tunnel started (start+next)");
				}
			}
			d->set_active_tab(ctx->tab);
			obs_data_t *s = obs_source_get_settings(d->context);
			if (s) {
				obs_data_set_int(s, "active_tab", ctx->tab);
				obs_data_release(s);
			}
			return true;
		}

		// WebSocket サーバーを停止し、状態表示を更新する。
		bool cb_ws_server_stop(obs_properties_t *, obs_property_t *, void *priv) {
			auto *d = static_cast<DelayStreamData *>(priv);
			if (!d || !d->router_running.load()) return false;
			d->router.stop();
			d->router_running.store(false);
			blog(LOG_INFO, "[obs-delay-stream] WebSocket server stopped");
			d->request_props_refresh_for_tabs({TAB_AUDIO_STREAMING, TAB_URL_SHARING}, "cb_ws_server_stop");
			return false;
		}

		// cloudflared トンネル起動を要求する。
		bool cb_tunnel_start(obs_properties_t *, obs_property_t *, void *priv) {
			auto *d = static_cast<DelayStreamData *>(priv);
			if (!d || !d->context) return false;
			obs_data_t *s = obs_source_get_settings(d->context);
			if (!s) return false;
			const std::string exe = build_exe_path_hint(
				s,
				ods::plugin::kCloudflaredExePathModeKey,
				ods::plugin::kCloudflaredExePathKey);
			obs_data_release(s);
			int ws_port = d->ws_port.load(std::memory_order_relaxed);
			d->tunnel.start(exe, ws_port);
			d->request_props_refresh_for_tabs({TAB_TUNNEL, TAB_URL_SHARING}, "cb_tunnel_start");
			return false;
		}

		// cloudflared バイナリを既定配置先へ非同期ダウンロードする。
		bool cb_cloudflared_download(obs_properties_t *, obs_property_t *, void *priv) {
			auto *d = static_cast<DelayStreamData *>(priv);
			if (!d || !d->context) return false;

			bool expected = false;
			if (!d->manual_cloudflared_download_running.compare_exchange_strong(
					expected,
					true,
					std::memory_order_acq_rel)) {
				return false;
			}
			d->request_props_refresh_for_tabs({TAB_TUNNEL}, "cb_cloudflared_download.begin");

			auto life = std::weak_ptr<std::atomic<bool>>(d->life_token);
			try {
				std::thread([d, life]() {
					std::string out_path;
					std::string err;
					const bool  ok =
						ods::tunnel::TunnelManager::ensure_auto_cloudflared_path(out_path, err);
					if (!ok) {
						blog(LOG_WARNING,
							 "[obs-delay-stream] cloudflared download failed: %s",
							 err.c_str());
					}

					struct UiCtx {
						std::weak_ptr<std::atomic<bool>> life_token;
						DelayStreamData                 *data;
					};
					auto ui_ctx = std::make_unique<UiCtx>(UiCtx{life, d});
					obs_queue_task(OBS_TASK_UI, [](void *param) {
						auto ctx = std::unique_ptr<UiCtx>(static_cast<UiCtx *>(param));
						auto token = ctx->life_token.lock();
						if (!token || !token->load(std::memory_order_acquire)) return;

						ctx->data->manual_cloudflared_download_running.store(false, std::memory_order_release);
						ods::plugin::maybe_persist_cloudflared_path_after_auto_ready(ctx->data->context);
						ctx->data->request_props_refresh_for_tabs({TAB_TUNNEL}, "cb_cloudflared_download.done"); }, ui_ctx.release(), false);
				}).detach();
			} catch (...) {
				d->manual_cloudflared_download_running.store(false, std::memory_order_release);
				d->request_props_refresh_for_tabs({TAB_TUNNEL}, "cb_cloudflared_download.spawn_error");
			}
			return false;
		}

		// cloudflared トンネルを停止する。
		bool cb_tunnel_stop(obs_properties_t *, obs_property_t *, void *priv) {
			auto *d = static_cast<DelayStreamData *>(priv);
			if (!d) return false;
			d->tunnel.stop();
			d->request_props_refresh_for_tabs({TAB_TUNNEL, TAB_URL_SHARING}, "cb_tunnel_stop");
			return false;
		}

		// WS 一括計測を開始する（計測済みチャンネルはスキップ）。
		bool cb_flow_start(obs_properties_t *, obs_property_t *, void *priv) {
			auto *d = static_cast<DelayStreamData *>(priv);
			if (!d) return false;
			if (d->ws_any_measuring() || d->rtsp_e2e_measure.is_measuring())
				return false;
			const std::string sid = d->get_stream_id();
			if (sid.empty()) return false;

			const int pings = d->ping_count_setting.load(std::memory_order_relaxed);

			// 計測対象チャンネルを列挙
			int measure_count = 0;
			for (int i = 0; i < d->delay.sub_ch_count; ++i) {
				if (d->router.client_count(i) > 0 && !d->delay.channels[i].ws_measured)
					++measure_count;
			}
			if (measure_count == 0) return false;

			// バッチ進捗を初期化
			d->ws_batch_progress.reset();
			d->ws_batch_progress.ping_total_count.store(
				measure_count * pings,
				std::memory_order_relaxed);

			// ping 送信ごとにプログレスバーを直接更新
			d->router.on_any_ping_sent = [d](const std::string &sid_cb, int, int) {
				if (sid_cb != d->get_stream_id()) return;
				const int sent  = d->ws_batch_progress.ping_sent_count.fetch_add(
									  1,
									  std::memory_order_relaxed) +
								  1;
				const int total = d->ws_batch_progress.ping_total_count.load(
					std::memory_order_relaxed);
				const int pct = (total > 0) ? (sent * 100 / total) : 0;
				update_flow_progress(d->context, pct);
			};

			// 各チャンネルの計測を開始（スタガー配置）
			int ch_index = 0;
			for (int i = 0; i < d->delay.sub_ch_count; ++i) {
				if (d->router.client_count(i) == 0 || d->delay.channels[i].ws_measured)
					continue;
				d->sub_channels[i].measure.start();
				d->router.start_measurement(i, pings, PING_INTV_MS, ch_index * PING_INTV_MS);
				++ch_index;
			}
			d->request_props_refresh_for_tabs({TAB_SYNC_LATENCY, TAB_FINE_ADJUST}, "cb_flow_start");
			return false;
		}

		// 全チャンネルの WS 計測結果をクリアする。
		bool cb_flow_clear_results(obs_properties_t *, obs_property_t *, void *priv) {
			auto *d = static_cast<DelayStreamData *>(priv);
			if (!d || !d->context) return false;
			obs_data_t *s = obs_source_get_settings(d->context);
			if (!s) return false;
			ods::model::SettingsRepo repo(s);
			for (int i = 0; i < MAX_SUB_CH; ++i) {
				repo.set_ch_measured_ms(i, 0);
				repo.set_ch_ws_measured(i, false);
			}
			obs_source_update(d->context, s);
			obs_data_release(s);
			for (int i = 0; i < MAX_SUB_CH; ++i)
				d->sub_channels[i].measure.reset();
			d->ws_batch_progress.reset();
			d->request_props_refresh_for_tabs({TAB_SYNC_LATENCY, TAB_RTSP_LATENCY, TAB_FINE_ADJUST}, "cb_flow_clear_results");
			return false;
		}

		// RTSP E2E 計測を開始する。
		bool cb_flow_start_rtsp_e2e(obs_properties_t *, obs_property_t *, void *priv) {
			auto *d = static_cast<DelayStreamData *>(priv);
			if (!d || !d->context) return false;
			if (d->ws_any_measuring() || d->rtsp_e2e_measure.is_measuring())
				return false;

			obs_data_t *settings = obs_source_get_settings(d->context);
			if (!settings) return false;
			bool auto_mode = obs_data_get_bool(settings, "rtmp_url_auto");
			if (auto_mode) {
				ods::plugin::maybe_autofill_rtmp_url(settings, true);
			}
			const bool  use_rtmp_for_rtsp = obs_data_get_bool(settings, ods::plugin::kRtspUseRtmpUrlKey);
			std::string rtsp_url;
			if (use_rtmp_for_rtsp) {
				const char *raw_rtmp_url = obs_data_get_string(settings, "rtmp_url");
				rtsp_url                 = to_rtsp_url_from_rtmp(raw_rtmp_url ? raw_rtmp_url : "");
				if (!rtsp_url.empty()) {
					obs_data_set_string(settings, ods::plugin::kRtspUrlKey, rtsp_url.c_str());
				}
			} else {
				const char *raw_rtsp_url = obs_data_get_string(settings, ods::plugin::kRtspUrlKey);
				rtsp_url                 = raw_rtsp_url ? raw_rtsp_url : "";
			}
			std::string ffmpeg_path = build_exe_path_hint(
				settings,
				ods::plugin::kFfmpegExePathModeKey,
				ods::plugin::kFfmpegExePathKey);
			obs_data_release(settings);

			// バリデーション
			std::string error_msg;
			if (rtsp_url.empty()) {
				error_msg = "RTSP URL が未設定です。";
			} else if (!ods::plugin::is_obs_streaming_active()) {
				error_msg = "OBS 配信が開始されていません。";
			}
			if (!error_msg.empty()) {
				d->rtsp_e2e_measure.set_last_error(error_msg);
				d->request_props_refresh_for_tabs({TAB_RTSP_LATENCY}, "cb_flow_start_rtsp_e2e.error");
				return false;
			}

			// 測定モードを読み取り、ミュートモードならフラグを立てる
			const int probe_mode = static_cast<int>(
				obs_data_get_int(settings, ods::plugin::kRtspE2eProbeModeKey));
			d->probe_mute_active.store(probe_mode == 1, std::memory_order_release);

			// 進捗を初期化
			d->rtsp_e2e_measure.set_last_error("");
			d->rtsp_e2e_measure.set_progress(0, RTSP_E2E_MEASURE_SETS_DEFAULT);
			d->inject_impulse.store(false, std::memory_order_release);
			d->rtsp_e2e_measure.set_cached_url(rtsp_url);

			// prober コールバックを設定
			auto &prober    = d->rtsp_e2e_measure.prober;
			prober.on_ready = [d]() {
				d->inject_impulse.store(true, std::memory_order_release);
				d->rtsp_e2e_measure.prober.notify_impulse_sent(
					std::chrono::steady_clock::now());
			};
			prober.on_progress = [d](int completed, int total) {
				d->rtsp_e2e_measure.set_progress(completed, total);
				const int pct = (total > 0) ? (completed * 100 / total) : 0;
				update_flow_progress(d->context, pct);
			};
			prober.on_result = [d](ods::network::RtspE2eResult r) {
				d->probe_mute_active.store(false, std::memory_order_release);
				d->rtsp_e2e_measure.apply_result(r);
				if (r.valid) {
					// 計測結果を Model に反映し、UI スレッドで永続化する
					d->delay.measured_rtsp_e2e_ms = static_cast<int>(std::lround(r.latency_ms));
					d->delay.rtsp_e2e_measured    = true;
					d->rtsp_e2e_measure.set_progress(
						d->rtsp_e2e_measure.total_sets(),
						d->rtsp_e2e_measure.total_sets());

					// UI スレッドで OBS 設定を永続化してディレイ再計算
					struct Ctx {
						std::weak_ptr<std::atomic<bool>> life;
						DelayStreamData                 *d;
					};
					auto c = std::make_unique<Ctx>(Ctx{d->life_token, d});
					obs_queue_task(OBS_TASK_UI, [](void *p) {
						auto ctx = std::unique_ptr<Ctx>(static_cast<Ctx *>(p));
						auto life = ctx->life.lock();
						if (!life || !life->load(std::memory_order_acquire)) return;
						auto *dd = ctx->d;
						obs_data_t *s = obs_source_get_settings(dd->context);
						if (s) {
							ods::model::SettingsRepo repo(s);
							repo.set_measured_rtsp_e2e_ms(dd->delay.measured_rtsp_e2e_ms);
							repo.set_rtsp_e2e_measured(dd->delay.rtsp_e2e_measured);
							obs_source_update(dd->context, s);
							obs_data_release(s);
						}
						ods::plugin::recalc_all_delays(dd);
						dd->request_props_refresh_for_tabs({TAB_RTSP_LATENCY, TAB_FINE_ADJUST}, "rtsp_e2e.on_result.apply"); }, c.release(), false);
				} else {
					d->rtsp_e2e_measure.set_last_error(r.error_msg);
					d->request_props_refresh_for_tabs({TAB_RTSP_LATENCY}, "rtsp_e2e.on_result.error");
				}
			};

			if (!prober.start(rtsp_url, ffmpeg_path)) {
				d->rtsp_e2e_measure.set_last_error("RTSP E2E 計測の開始に失敗しました。");
				d->request_props_refresh_for_tabs({TAB_RTSP_LATENCY}, "cb_flow_start_rtsp_e2e.start_failed");
				return false;
			}
			d->request_props_refresh_for_tabs({TAB_RTSP_LATENCY}, "cb_flow_start_rtsp_e2e.started");
			return false;
		}

		// ffmpeg バイナリを既定配置先へ非同期ダウンロードする。
		bool cb_ffmpeg_download(obs_properties_t *, obs_property_t *, void *priv) {
			auto *d = static_cast<DelayStreamData *>(priv);
			if (!d || !d->context) return false;

			bool expected = false;
			if (!d->manual_ffmpeg_download_running.compare_exchange_strong(
					expected,
					true,
					std::memory_order_acq_rel)) {
				return false;
			}
			d->request_props_refresh_for_tabs({TAB_RTSP_LATENCY}, "cb_ffmpeg_download.begin");

			auto life = std::weak_ptr<std::atomic<bool>>(d->life_token);
			try {
				std::thread([d, life]() {
					std::string out_path;
					std::string err;
					const bool  ok =
						ods::network::RtspE2eProber::ensure_auto_ffmpeg_path(out_path, err);
					if (!ok) {
						blog(LOG_WARNING,
							 "[obs-delay-stream] ffmpeg download failed: %s",
							 err.c_str());
					}

					struct UiCtx {
						std::weak_ptr<std::atomic<bool>> life_token;
						DelayStreamData                 *data;
					};
					auto ui_ctx = std::make_unique<UiCtx>(UiCtx{life, d});
					obs_queue_task(OBS_TASK_UI, [](void *param) {
						auto ctx = std::unique_ptr<UiCtx>(static_cast<UiCtx *>(param));
						auto token = ctx->life_token.lock();
						if (!token || !token->load(std::memory_order_acquire)) return;

						ctx->data->manual_ffmpeg_download_running.store(false, std::memory_order_release);
						ctx->data->request_props_refresh_for_tabs({TAB_RTSP_LATENCY}, "cb_ffmpeg_download.done"); }, ui_ctx.release(), false);
				}).detach();
			} catch (...) {
				d->manual_ffmpeg_download_running.store(false, std::memory_order_release);
				d->request_props_refresh_for_tabs({TAB_RTSP_LATENCY}, "cb_ffmpeg_download.spawn_error");
			}
			return false;
		}

		// 計測を中断して状態を初期化する。
		bool cb_flow_reset(obs_properties_t *, obs_property_t *, void *priv) {
			auto *d = static_cast<DelayStreamData *>(priv);
			if (!d) return false;
			for (int i = 0; i < MAX_SUB_CH; ++i)
				d->sub_channels[i].measure.reset();
			d->ws_batch_progress.reset();
			d->rtsp_e2e_measure.cancel();
			d->inject_impulse.store(false, std::memory_order_release);
			d->probe_mute_active.store(false, std::memory_order_release);
			d->request_props_refresh_for_tabs({TAB_SYNC_LATENCY, TAB_RTSP_LATENCY, TAB_FINE_ADJUST}, "cb_flow_reset");
			return false;
		}

		// 「高度な設定」チェックボックスのトグルでグループ表示を切り替える。
		// return true は OBS の RefreshProperties を起動し、同ページ内の
		// カスタムウィジェットを破壊するため、直後に inject を再スケジュールする。
		bool cb_show_advanced_changed(void *priv, obs_properties_t *props, obs_property_t *, obs_data_t *settings) {
			auto *d = static_cast<DelayStreamData *>(priv);
			if (!props || !settings || !d) return false;
			bool show = obs_data_get_bool(settings, "show_advanced");
			if (auto *p = obs_properties_get(props, "grp_stream"))
				obs_property_set_visible(p, show);
			props_ui_with_preserved_scroll([d]() {
				if (!d || !d->context) return;
				schedule_color_button_row_inject(d->context);
				schedule_pulldown_row_inject(d->context);
				schedule_stepper_inject(d->context);
				schedule_help_callout_inject(d->context);
				schedule_button_bar_inject(d->context);
			});
			return true;
		}

		// stream_id の有無に応じて関連ボタンの有効状態を切り替える。
		bool cb_stream_id_changed(void *priv, obs_properties_t *props, obs_property_t *, obs_data_t *settings) {
			auto *d = static_cast<DelayStreamData *>(priv);
			if (!props || !settings) return false;
			const char *sid     = obs_data_get_string(settings, "stream_id");
			bool        has_sid = (sid && *sid);
			if (auto *p = obs_properties_get(props, "ws_server_start_btn")) {
				obs_property_set_enabled(p, has_sid);
			}
			if (auto *p = obs_properties_get(props, "ws_server_start_note_sid")) {
				obs_property_set_visible(p, !has_sid);
			}
			props_ui_with_preserved_scroll([d]() {
				if (!d || !d->context) return;
				schedule_color_button_row_inject(d->context);
				schedule_pulldown_row_inject(d->context);
				schedule_stepper_inject(d->context);
				schedule_help_callout_inject(d->context);
				schedule_button_bar_inject(d->context);
			});
			return true;
		}

		// コーデック変更時に表示項目とカスタムウィジェットを再同期する。
		bool cb_audio_codec_changed(void *priv, obs_properties_t *props, obs_property_t *, obs_data_t *settings) {
			auto *d = static_cast<DelayStreamData *>(priv);
			ods::plugin::apply_codec_option_visibility(props, settings);
			props_ui_with_preserved_scroll([d]() {
				if (!d || !d->context) return;
				schedule_color_button_row_inject(d->context);
				schedule_pulldown_row_inject(d->context);
				schedule_stepper_inject(d->context);
				schedule_help_callout_inject(d->context);
				schedule_button_bar_inject(d->context);
			});
			return true;
		}

		// ============================================================
		// private 補助関数
		// ============================================================

		// RTSP E2E 計測セクションのボタン群と状態表示を構築する。
		void add_flow_rtsp_e2e_measure_section(obs_properties_t *grp, DelayStreamData *d) {
			if (!grp || !d) return;

			const bool is_ws_measuring   = d->ws_any_measuring();
			const bool is_rtsp_measuring = d->rtsp_e2e_measure.is_measuring();

			// 測定モード選択
			obs_property_t *mode_list = obs_properties_add_list(
				grp,
				ods::plugin::kRtspE2eProbeModeKey,
				T_("RtspE2eProbeMode"),
				OBS_COMBO_TYPE_LIST,
				OBS_COMBO_FORMAT_INT);
			obs_property_list_add_int(mode_list, T_("RtspE2eProbeMute"), 1);
			obs_property_list_add_int(mode_list, T_("RtspE2eProbeMix"), 0);
			obs_property_set_enabled(mode_list, !is_rtsp_measuring);
			const bool obs_streaming      = ods::plugin::is_obs_streaming_active();
			const bool can_start          = obs_streaming && !is_ws_measuring && !is_rtsp_measuring;
			const bool rtsp_start_enabled = can_start;
			const bool rtsp_stop_enabled  = is_rtsp_measuring;

			// 計測完了状態ではヒントを出さない。
			const auto  rtsp_result = d->rtsp_e2e_measure.result();
			const bool  rtsp_done   = d->delay.rtsp_e2e_measured || rtsp_result.valid;
			const char *rtsp_hint   = nullptr;
			if (!rtsp_start_enabled && !rtsp_stop_enabled && !rtsp_done) {
				if (is_ws_measuring)
					rtsp_hint = T_("RtspE2eHintWsMeasuring");
				else if (!obs_streaming)
					rtsp_hint = T_("RtspE2eHintStartStreaming");
			}

			const ObsColorButtonSpec flow_rtsp_e2e_buttons[] = {
				{
					"flow_rtsp_e2e_measure_start_btn",
					T_("FlowMeasureStartShort"),
					cb_flow_start_rtsp_e2e,
					d,
					rtsp_start_enabled,
					nullptr,
					nullptr,
				},
				{
					"flow_rtsp_e2e_measure_stop_btn",
					T_("FlowMeasureStopShort"),
					cb_flow_reset,
					d,
					rtsp_stop_enabled,
					nullptr,
					nullptr,
				},
			};
			obs_properties_add_color_button_row(
				grp,
				"flow_rtsp_e2e_measure_controls_row",
				T_("RtspE2eMeasure"),
				flow_rtsp_e2e_buttons,
				sizeof(flow_rtsp_e2e_buttons) / sizeof(flow_rtsp_e2e_buttons[0]),
				nullptr,
				rtsp_hint,
				"#14b8a6",
				rtsp_hint ? kWarningTextColorLight : nullptr,
				rtsp_hint ? kWarningTextColorDark : nullptr);

			{
				int         pct = 0;
				std::string bar_text_str;
				const char *bar_text = T_("FlowNotMeasured");
				if (is_rtsp_measuring) {
					const int total     = d->rtsp_e2e_measure.total_sets();
					const int completed = d->rtsp_e2e_measure.completed_sets();
					pct                 = (total > 0) ? (completed * 100 / total) : 0;
					bar_text            = T_("StatusMeasuring");
				} else if (rtsp_result.valid) {
					pct          = 100;
					bar_text_str = string_printf(
						T_("RtspE2eResultFmt"),
						rtsp_result.latency_ms,
						rtsp_result.min_latency_ms,
						rtsp_result.max_latency_ms);
					bar_text = bar_text_str.c_str();
				} else if (!d->rtsp_e2e_measure.last_error().empty()) {
					bar_text = T_("RtspE2eFailed");
				} else if (d->delay.rtsp_e2e_measured) {
					pct          = 100;
					bar_text_str = string_printf(T_("MeasuredRtspE2eFmt"), d->delay.measured_rtsp_e2e_ms);
					bar_text     = bar_text_str.c_str();
				}
				obs_properties_add_flow_progress(grp, "flow_s4_status", nullptr, pct, bar_text);
			}

			{
				const std::string err = d->rtsp_e2e_measure.last_error();
				if (!err.empty()) {
					obs_properties_add_text(
						grp,
						"flow_s4_error_detail",
						err.c_str(),
						OBS_TEXT_INFO);
				}
			}
		}

		// WS 計測パネル全体（接続状況/操作/進捗）を構築する。
		void build_flow_panel(obs_properties_t *grp, DelayStreamData *d) {
			if (!grp || !d) return;
			int sub_count = d->delay.sub_ch_count;

			const bool is_ws_measuring   = d->ws_any_measuring();
			const bool is_rtsp_measuring = d->rtsp_e2e_measure.is_measuring();

			obs_data_t *s               = obs_source_get_settings(d->context);
			auto        format_sub_name = [s](int ch) -> std::string {
				if (!s) return "Ch." + std::to_string(ch + 1);
				const auto  memo_key = make_sub_memo_key(ch);
				const char *memo     = obs_data_get_string(s, memo_key.data());
				if (memo && *memo) return std::string(memo);
				return "Ch." + std::to_string(ch + 1);
			};

			int connected_count = 0;
			for (int i = 0; i < sub_count; ++i) {
				if (d->router.client_count(i) > 0)
					++connected_count;
			}

			// 接続中で未計測のチャンネルが1つもなければ計測不要
			int measurable_count = 0;
			for (int i = 0; i < sub_count; ++i) {
				if (d->router.client_count(i) <= 0) continue;
				if (d->delay.channels[i].ws_measured) continue;
				++measurable_count;
			}

			// 保存済み計測結果の有無（リセットボタンの有効条件に使用）
			bool has_any_ws_measured = false;
			for (int i = 0; i < sub_count; ++i) {
				if (d->delay.channels[i].ws_measured) {
					has_any_ws_measured = true;
					break;
				}
			}

			const bool can_start_ws  = !is_ws_measuring && !is_rtsp_measuring;
			const bool can_clear     = can_start_ws && has_any_ws_measured;
			const bool start_enabled = (can_start_ws && measurable_count > 0);
			const bool stop_enabled  = is_ws_measuring;

			// 開始・停止が両方無効のとき、ユーザに次のアクションを促すヒントを表示する。
			// 計測完了状態（全チャンネル計測済み）ではヒントを出さない。ただし
			// サーバー未起動や RTSP 計測中のヒントは計測完了状態でも表示する。
			const bool  ws_all_done = (has_any_ws_measured && measurable_count == 0);
			const char *ws_hint     = nullptr;
			if (!start_enabled && !stop_enabled) {
				if (is_rtsp_measuring)
					ws_hint = T_("FlowHintRtspMeasuring");
				else if (!d->router_running.load())
					ws_hint = T_("FlowHintStartWsServer");
				else if (!ws_all_done && connected_count == 0)
					ws_hint = T_("FlowHintWaitConnection");
			}

			const ObsColorButtonSpec flow_measure_buttons[] = {
				{
					"flow_measure_start_btn",
					T_("FlowMeasureStartShort"),
					cb_flow_start,
					d,
					start_enabled,
					nullptr,
					nullptr,
				},
				{
					"flow_measure_stop_btn",
					T_("FlowMeasureStopShort"),
					cb_flow_reset,
					d,
					stop_enabled,
					nullptr,
					nullptr,
				},
				{
					"flow_measure_clear_btn",
					T_("FlowClearResults"),
					cb_flow_clear_results,
					d,
					can_clear,
					nullptr,
					nullptr,
				},
			};
			obs_properties_add_color_button_row(
				grp,
				"flow_measure_controls_row",
				T_("FlowMeasureLabel"),
				flow_measure_buttons,
				sizeof(flow_measure_buttons) / sizeof(flow_measure_buttons[0]),
				nullptr,
				ws_hint,
				"#2563eb",
				ws_hint ? kWarningTextColorLight : nullptr,
				ws_hint ? kWarningTextColorDark : nullptr);

			obs_properties_add_bool(grp, "auto_measure", T_("AutoMeasure"));

			{
				int         pct      = 0;
				const char *bar_text = T_("FlowNotMeasured");
				if (is_ws_measuring) {
					const int total = d->ws_batch_progress.ping_total_count.load(
						std::memory_order_relaxed);
					const int sent = d->ws_batch_progress.ping_sent_count.load(
						std::memory_order_relaxed);
					pct      = (total > 0) ? (sent * 100 / total) : 0;
					bar_text = T_("StatusMeasuring");
				} else if (has_any_ws_measured) {
					pct      = 100;
					bar_text = T_("FlowMeasureDoneShort");
				}
				obs_properties_add_flow_progress(
					grp,
					"flow_s1_status",
					nullptr,
					pct,
					bar_text);
			}

			// チャンネル一覧テーブル（接続状況＋計測結果）
			{
				std::vector<std::string>          name_buf(sub_count);
				std::vector<FlowTableChannelInfo> table_channels(sub_count);
				for (int i = 0; i < sub_count; ++i) {
					name_buf[i]  = format_sub_name(i);
					auto &tc     = table_channels[i];
					tc.name      = name_buf[i].c_str();
					tc.connected = (d->router.client_count(i) > 0);
					// 計測済みなら保存値を表示、未計測なら -1
					if (d->delay.channels[i].ws_measured)
						tc.measured_ms = d->delay.channels[i].measured_ms;
					else
						tc.measured_ms = -1;
				}

				FlowTableLabels tbl_labels{};
				tbl_labels.hdr_ch              = T_("FlowTableColCh");
				tbl_labels.hdr_name            = T_("FlowTableColName");
				tbl_labels.hdr_status          = T_("FlowTableColStatus");
				tbl_labels.hdr_result          = T_("FlowTableColResult");
				tbl_labels.status_connected    = T_("FlowTableStatusConnected");
				tbl_labels.status_disconnected = T_("FlowTableStatusDisconnected");
				tbl_labels.result_failed       = T_("FlowChFailed");

				obs_properties_add_flow_table(
					grp,
					"flow_table",
					sub_count,
					table_channels.data(),
					tbl_labels);
			}

			if (s) obs_data_release(s);
		}

	} // namespace

	// タブ選択を設定へ反映し、プロパティを再描画する。
	bool cb_select_tab(obs_properties_t *, obs_property_t *, void *priv) {
		auto *ctx = static_cast<ods::plugin::TabCtx *>(priv);
		if (!ctx || !ctx->d || !ctx->d->context) return false;
		ctx->d->set_active_tab(ctx->tab);
		obs_data_t *s = obs_source_get_settings(ctx->d->context);
		if (s) {
			obs_data_set_int(s, "active_tab", ctx->tab);
			obs_data_release(s);
		}
		return true;
	}

	// ============================================================
	// public add_* 関数
	// ============================================================

	void add_tab_selector_row(obs_properties_t *props, DelayStreamData *d, int active_tab) {
		if (!props || !d) return;
		static const char *const kActionNames[] = {
			"tab_act_0",
			"tab_act_1",
			"tab_act_2",
			"tab_act_3",
			"tab_act_4",
			"tab_act_5",
			"tab_act_6",
		};
		static const char *const kLocaleKeys[] = {
			"TabPerformerNames",
			"TabTunnel",
			"TabAudioStreaming",
			"TabUrlSharing",
			"TabSyncLatency",
			"TabRtmpLatency",
			"TabFineAdjust",
		};
		const char *kInactiveBg = "auto";
		if (active_tab < 0 || active_tab >= TAB_COUNT) active_tab = 0;

		ObsColorButtonSpec buttons[TAB_COUNT];
		for (int i = 0; i < TAB_COUNT; ++i) {
			buttons[i] = {
				kActionNames[i],
				T_(kLocaleKeys[i]),
				cb_select_tab,
				&d->tab_btn_ctx[i],
				true,
				(i == active_tab) ? nullptr : kInactiveBg,
				nullptr,
			};
		}
		obs_properties_add_color_button_row(
			props,
			"tab_selector_row",
			"",
			buttons,
			TAB_COUNT);
	}

	void add_next_tab_button_bar(obs_properties_t *props, DelayStreamData *d, int next_tab, bool enabled) {
		if (!props || !d) return;
		if (next_tab < 0 || next_tab >= TAB_COUNT) return;

		// プロパティ名と action 名をタブ番号で一意にする。
		const std::string prop_name   = "next_tab_bar_" + std::to_string(next_tab);
		const std::string action_name = "next_tab_act_" + std::to_string(next_tab);

		const ObsButtonBarSpec next_btn = {
			action_name.c_str(),
			T_("BtnNextWsTab"),
			cb_select_tab,
			&d->tab_btn_ctx[next_tab],
			enabled,
		};
		obs_properties_add_button_bar(
			props,
			prop_name.c_str(),
			"",
			nullptr,
			0,
			&next_btn,
			1);
	}

	void add_ws_next_button_bar(obs_properties_t *props, DelayStreamData *d, bool has_sid) {
		if (!props || !d) return;
		const bool ws_running = d->router_running.load();

		if (ws_running) {
			// 起動済み: 通常の「次へ」ボタン
			add_next_tab_button_bar(props, d, TAB_URL_SHARING);
		} else {
			// 未起動: 「起動して次へ」ボタン
			const ObsButtonBarSpec start_next_btn = {
				"ws_start_next_act",
				T_("BtnStartWsAndNext"),
				cb_ws_start_and_goto_tab,
				&d->tab_btn_ctx[TAB_URL_SHARING],
				has_sid,
			};
			obs_properties_add_button_bar(
				props,
				"ws_start_next_bar",
				"",
				nullptr,
				0,
				&start_next_btn,
				1);
		}
	}

	void add_tunnel_next_button_bar(obs_properties_t *props, DelayStreamData *d) {
		if (!props || !d) return;
		TunnelState ts             = d->tunnel.state();
		const bool  tunnel_running = (ts == TunnelState::Running);
		const bool  tunnel_busy    = (ts == TunnelState::Starting);

		if (tunnel_running) {
			add_next_tab_button_bar(props, d, TAB_AUDIO_STREAMING);
		} else {
			// 右寄せ: 「スキップ」「起動して次へ」
			const ObsButtonBarSpec right_btns[] = {
				{
					"tunnel_skip_act",
					T_("BtnSkipTab"),
					cb_select_tab,
					&d->tab_btn_ctx[TAB_AUDIO_STREAMING],
					true,
				},
				{
					"tunnel_start_next_act",
					T_("BtnStartTunnelAndNext"),
					cb_tunnel_start_and_goto_tab,
					&d->tab_btn_ctx[TAB_AUDIO_STREAMING],
					!tunnel_busy,
				},
			};
			obs_properties_add_button_bar(
				props,
				"tunnel_start_next_bar",
				"",
				nullptr,
				0,
				right_btns,
				2);
		}
	}

	void add_plugin_group(obs_properties_t *props, DelayStreamData *d) {
		if (!props || !d) return;
		obs_properties_t *grp = obs_properties_create();

		obs_property_t *about_p = obs_properties_add_text(grp, "about_info", "", OBS_TEXT_INFO);
		obs_property_set_long_description(about_p,
										  "v" PLUGIN_VERSION " | (C) 2026 Mazzn1987, Chigiri Tsutsumi | GPL 2.0+ | "
										  "<a href=\"https://github.com/MZ1987Records/obs-delay-stream\">GitHub</a> | "
										  "<a href=\"https://mz1987records.booth.pm/items/8134637\">Booth</a>");
		obs_property_text_set_info_word_wrap(about_p, false);
		obs_property_t *schema_p = obs_properties_add_int(
			grp,
			ods::plugin::kSettingsSchemaVersionKey,
			"",
			0,
			999,
			1);
		obs_property_set_visible(schema_p, false);
		obs_property_set_enabled(schema_p, false);
		obs_property_t *saved_ver_p = obs_properties_add_text(
			grp,
			ods::plugin::kSettingsSavedVersionKey,
			"",
			OBS_TEXT_DEFAULT);
		obs_property_set_visible(saved_ver_p, false);
		obs_property_set_enabled(saved_ver_p, false);

		if (d->update_check.status.load(std::memory_order_acquire) ==
			UpdateCheckStatus::UpdateAvailable) {
			const std::string latest_version = d->update_check.latest_version();
			if (!latest_version.empty()) {
				const std::string update_notice =
					string_printf(T_("UpdateAvailableNoticeFmt"), latest_version.c_str());
				obs_property_t *update_notice_p = obs_properties_add_text(
					grp,
					"update_available_notice_top",
					update_notice.c_str(),
					OBS_TEXT_INFO);
				obs_property_text_set_info_word_wrap(update_notice_p, true);
			}
		}

		if (d->is_duplicate_instance) {
			obs_properties_add_text(
				grp,
				"duplicate_instance_warning",
				"複数の obs-delay-stream フィルタを使用することはできません。",
				OBS_TEXT_INFO);
		} else if (d->has_settings_mismatch) {
			obs_property_t *warn_p = obs_properties_add_text(
				grp,
				"settings_mismatch_warning",
				T_("SettingsMismatchWarning"),
				OBS_TEXT_INFO);
			obs_property_text_set_info_word_wrap(warn_p, true);
		} else {
			if ((obs_get_version() >> 24) < 32) {
				obs_property_t *ver_warn_p = obs_properties_add_text(
					grp,
					"obs_version_warning_top",
					T_("ObsVersionWarning"),
					OBS_TEXT_INFO);
				obs_property_text_set_info_word_wrap(ver_warn_p, true);
			}
			int64_t sync_offset_ns = 0;
			if (ods::plugin::try_get_parent_audio_sync_offset_ns(d->context, sync_offset_ns) &&
				sync_offset_ns != REQUIRED_AUDIO_SYNC_OFFSET_NS) {
				obs_property_t *warn_p = obs_properties_add_text(
					grp,
					"audio_sync_offset_warning_top",
					T_("AudioSyncOffsetWarning"),
					OBS_TEXT_INFO);
				obs_property_text_set_info_word_wrap(warn_p, true);
			}
		}

		if (!d->is_warning_only_instance()) {
			const bool  ws_on     = d->router_running.load();
			const bool  ws_paused = !d->ws_send_enabled.load();
			const bool  ws_no_dly = !d->enabled.load();
			const bool  ws_warn   = ws_on && (ws_paused || ws_no_dly);
			TunnelState ts_plugin = d->tunnel.state();
			bool        tun_on    = (ts_plugin == TunnelState::Running);
			bool        tun_busy  = (ts_plugin == TunnelState::Starting);
			const char *ws_color  = ws_on ? (ws_warn ? UI_COLOR_STATUS_DOT_BUSY
													 : UI_COLOR_STATUS_DOT_OK)
										  : UI_COLOR_STATUS_DOT_OFF;
			const char *tun_color = tun_on     ? UI_COLOR_STATUS_DOT_OK
									: tun_busy ? UI_COLOR_STATUS_DOT_BUSY
											   : UI_COLOR_STATUS_DOT_OFF;

			std::string ws_status = ws_on ? T_("StatusRunning") : T_("StatusStopped");
			if (ws_paused) ws_status += T_("WsPausedSuffix");
			const char       *tun_label   = tun_on     ? T_("StatusRunning")
											: tun_busy ? T_("TunnelStarting")
													   : T_("StatusStopped");
			const std::string status_html = string_printf(
				"<span style='color:%s'>●</span> %s %s"
				"&nbsp;&nbsp;|&nbsp;&nbsp;"
				"<span style='color:%s'>●</span> %s %s",
				tun_color,
				T_("PluginTunnelStatusLabel"),
				tun_label,
				ws_color,
				T_("PluginWsStatusLabel"),
				ws_status.c_str());
			obs_property_t *status_p =
				obs_properties_add_text(grp, "plugin_status_info", "", OBS_TEXT_INFO);
			obs_property_set_long_description(status_p, status_html.c_str());
			obs_property_text_set_info_word_wrap(status_p, false);
		}

		obs_properties_add_group(props, "grp_plugin", T_("Plugin"), OBS_GROUP_NORMAL, grp);
	}

	void add_stream_group(obs_properties_t *props, DelayStreamData *d) {
		if (!props || !d) return;

		// チェックボックス（グループの外に配置）
		obs_property_t *adv_p =
			obs_properties_add_bool(props, "show_advanced", T_("ShowAdvancedSettings"));
		obs_property_set_modified_callback2(adv_p, cb_show_advanced_changed, d);

		obs_data_t *settings = obs_source_get_settings(d->context);

		// auto_ip の値を設定に書き込み、編集不可テキストボックスで表示する
		if (settings)
			obs_data_set_string(settings, "auto_ip_info", d->auto_ip.c_str());

		obs_properties_t *grp = obs_properties_create();
		{
			obs_property_t *auto_ip_p =
				obs_properties_add_text(grp, "auto_ip_info", T_("AutoIpLabel"), OBS_TEXT_DEFAULT);
			obs_property_set_enabled(auto_ip_p, false);
		}
		obs_property_t *ip_p =
			obs_properties_add_text(grp, "host_ip_manual", T_("IpOverride"), OBS_TEXT_DEFAULT);
		obs_property_t *port_p =
			obs_properties_add_int(grp, "ws_port", T_("WsPort"), 1, 65535, 1);
		obs_property_t *sid_p =
			obs_properties_add_text(grp, "stream_id", T_("StreamId"), OBS_TEXT_DEFAULT);
		obs_property_set_modified_callback2(sid_p, cb_stream_id_changed, d);
		obs_property_set_enabled(sid_p, false);
		if (d->router_running.load()) {
			obs_property_set_enabled(ip_p, false);
			obs_property_set_enabled(port_p, false);
		}
		obs_property_t *grp_p =
			obs_properties_add_group(props, "grp_stream", T_("GroupStreamId"), OBS_GROUP_NORMAL, grp);

		// 初期状態: チェックされていなければグループを非表示
		bool show = settings ? obs_data_get_bool(settings, "show_advanced") : false;
		obs_property_set_visible(grp_p, show);
		if (settings) obs_data_release(settings);
	}

	void add_ws_group(obs_properties_t *props, DelayStreamData *d, bool has_sid) {
		if (!props || !d) return;
		bool ws_running = d->router_running.load();
		int  ws_port    = ws_running
							  ? (int)d->router.port()
							  : d->ws_port.load(std::memory_order_relaxed);

		obs_properties_t *grp     = obs_properties_create();
		obs_property_t   *codec_p = obs_properties_add_list(
			grp,
			"audio_codec",
			T_("AudioCodec"),
			OBS_COMBO_TYPE_LIST,
			OBS_COMBO_FORMAT_INT);
		obs_property_set_modified_callback2(codec_p, cb_audio_codec_changed, d);
		obs_property_list_add_int(codec_p, "Opus", 0);
		obs_property_list_add_int(codec_p, T_("CodecPcm"), 1);

		const ObsPulldownOptionSpec opus_bitrate_options[] = {
			{"24 kbps", 24},
			{"32 kbps", 32},
			{"48 kbps", 48},
			{"64 kbps", 64},
			{"96 kbps", 96},
			{"128 kbps", 128},
			{"160 kbps", 160},
			{"192 kbps", 192},
			{"256 kbps", 256},
			{"320 kbps", 320},
		};
		const ObsPulldownOptionSpec opus_sample_rate_options[] = {
			{"8000 Hz", 8000},
			{"12000 Hz", 12000},
			{"16000 Hz", 16000},
			{"24000 Hz", 24000},
			{"48000 Hz", 48000},
		};
		const ObsPulldownOptionSpec channel_mode_options[] = {
			{T_("AudioChannelStereo"), 0},
			{T_("AudioChannelMono"), 1},
		};
		uint32_t                    input_sr                    = d->sample_rate > 0 ? d->sample_rate : 48000;
		const uint32_t              pcm_sr_full                 = input_sr;
		const uint32_t              pcm_sr_half                 = (input_sr >= 2) ? (input_sr / 2) : 1;
		const uint32_t              pcm_sr_quarter              = (input_sr >= 4) ? (input_sr / 4) : 1;
		const std::string           pcm_sr_full_label           = string_printf("%u Hz", pcm_sr_full);
		const std::string           pcm_sr_half_label           = string_printf("%u Hz", pcm_sr_half);
		const std::string           pcm_sr_quarter_label        = string_printf("%u Hz", pcm_sr_quarter);
		const ObsPulldownOptionSpec quantization_bits_options[] = {
			{"8 bit", 8},
			{"16 bit", 16},
		};
		const ObsPulldownOptionSpec pcm_sample_rate_options[] = {
			{pcm_sr_full_label.c_str(), 1},
			{pcm_sr_half_label.c_str(), 2},
			{pcm_sr_quarter_label.c_str(), 4},
		};
		const ObsPulldownSpec opus_codec_specs[] = {
			{
				"opus_bitrate_kbps",
				nullptr,
				opus_bitrate_options,
				sizeof(opus_bitrate_options) / sizeof(opus_bitrate_options[0]),
				nullptr,
				nullptr,
				!ws_running,
			},
			{
				"opus_sample_rate",
				nullptr,
				opus_sample_rate_options,
				sizeof(opus_sample_rate_options) / sizeof(opus_sample_rate_options[0]),
				nullptr,
				nullptr,
				!ws_running,
				false,
			},
		};
		obs_properties_add_pulldown_row(
			grp,
			"audio_codec_opus_row",
			T_("AudioCodecOptions"),
			opus_codec_specs,
			sizeof(opus_codec_specs) / sizeof(opus_codec_specs[0]));

		const ObsPulldownSpec pcm_codec_specs[] = {
			{
				"pcm_downsample_ratio",
				nullptr,
				pcm_sample_rate_options,
				sizeof(pcm_sample_rate_options) / sizeof(pcm_sample_rate_options[0]),
				nullptr,
				nullptr,
				!ws_running,
			},
			{
				"quantization_bits",
				nullptr,
				quantization_bits_options,
				sizeof(quantization_bits_options) / sizeof(quantization_bits_options[0]),
				nullptr,
				nullptr,
				!ws_running,
				false,
			},
			{
				"audio_mono",
				nullptr,
				channel_mode_options,
				sizeof(channel_mode_options) / sizeof(channel_mode_options[0]),
				nullptr,
				nullptr,
				!ws_running,
				true,
			},
		};
		obs_properties_add_pulldown_row(
			grp,
			"audio_codec_pcm_row",
			T_("AudioCodecOptions"),
			pcm_codec_specs,
			sizeof(pcm_codec_specs) / sizeof(pcm_codec_specs[0]));
		obs_properties_add_stepper(
			grp,
			"playback_buffer_ms_stepper",
			T_("PlaybackBuffer"),
			"playback_buffer_ms",
			PLAYBACK_BUFFER_MIN_MS,
			PLAYBACK_BUFFER_MAX_MS,
			PLAYBACK_BUFFER_DEFAULT_MS,
			0,
			" ms",
			true,
			7,
			"#4b5563");

		if (ws_running) {
			obs_property_set_enabled(codec_p, false);
		}

		if (d->context) {
			obs_data_t *s = obs_source_get_settings(d->context);
			if (s) {
				ods::plugin::apply_codec_option_visibility(grp, s);
				obs_data_release(s);
			}
		}

		const ObsColorButtonSpec ws_buttons[] = {
			{
				"ws_server_start_btn",
				T_("WsServerStartShort"),
				cb_ws_server_start,
				d,
				(!ws_running && has_sid),
			},
			{
				"ws_server_stop_btn",
				T_("WsServerStopShort"),
				cb_ws_server_stop,
				d,
				(ws_running),
			},
		};
		obs_properties_add_color_button_row(
			grp,
			"ws_server_controls_row",
			T_("WsServerControls"),
			ws_buttons,
			sizeof(ws_buttons) / sizeof(ws_buttons[0]),
			ws_running ? UI_COLOR_STATUS_DOT_OK : UI_COLOR_STATUS_DOT_OFF,
			ws_running ? T_("StatusRunning") : T_("StatusStopped"));
		const std::string ws_firewall_note =
			string_printf(T_("WsFirewallNoteFmt"), ws_port);
		obs_properties_add_help_callout(grp, "ws_firewall_note", ws_firewall_note.c_str());

		obs_property_t *send_p = obs_properties_add_bool(grp, "ws_send_paused", T_("WsSendPause"));
		if (!ws_running) {
			obs_property_set_enabled(send_p, false);
		}
		obs_property_t *delay_p = obs_properties_add_bool(grp, "delay_disable", T_("DelayDisable"));
		if (!ws_running) {
			obs_property_set_enabled(delay_p, false);
		}
		obs_properties_add_group(props, "grp_ws", T_("WsGroupTitle"), OBS_GROUP_NORMAL, grp);
	}

	void add_tunnel_group(obs_properties_t *props, DelayStreamData *d) {
		if (!props || !d) return;
		TunnelState ts   = d->tunnel.state();
		std::string turl = d->tunnel.url();
		std::string terr = d->tunnel.error();

		obs_properties_t *grp                     = obs_properties_create();
		bool              cloudflared_downloading = d->tunnel.cloudflared_downloading();
		bool              cloudflared_dl_running =
			d->manual_cloudflared_download_running.load(std::memory_order_acquire);
		bool ws_running     = d->router_running.load();
		bool tunnel_running = (ts == TunnelState::Running);
		bool tunnel_busy =
			cloudflared_downloading || cloudflared_dl_running || (ts == TunnelState::Starting);
		std::string cloudflared_auto_path;
		const bool  cloudflared_auto_exists =
			ods::tunnel::TunnelManager::get_auto_cloudflared_path_if_exists(cloudflared_auto_path);
		const ObsPathModeRowSpec cloudflared_path_spec = {
			ods::plugin::kCloudflaredExePathModeKey,
			ods::plugin::kCloudflaredExePathKey,
			T_("ExePathModeAuto"),
			T_("ExePathModePath"),
			T_("ExePathModeAbsolute"),
			(cloudflared_downloading || cloudflared_dl_running) ? T_("PathModeDownloading") : T_("PathModeDownload"),
			cb_cloudflared_download,
			d,
			(!cloudflared_downloading && !cloudflared_dl_running),
			true,
			0,
			cloudflared_auto_exists,
			cloudflared_auto_path.c_str(),
		};
		obs_properties_add_path_mode_row(
			grp,
			"cloudflared_exe_path_row",
			T_("CloudflaredExePath"),
			cloudflared_path_spec);

		const ObsColorButtonSpec tunnel_buttons[] = {
			{
				"tunnel_start_btn",
				T_("TunnelStartShort"),
				cb_tunnel_start,
				d,
				(!tunnel_running && !tunnel_busy),
			},
			{
				"tunnel_stop_btn",
				T_("TunnelStopShort"),
				cb_tunnel_stop,
				d,
				tunnel_running,
			},
		};
		const char *tunnel_status_dot  = tunnel_running                  ? UI_COLOR_STATUS_DOT_OK
										 : (ts == TunnelState::Starting) ? UI_COLOR_STATUS_DOT_BUSY
																		 : UI_COLOR_STATUS_DOT_OFF;
		const char *tunnel_status_text = tunnel_running                  ? T_("StatusRunning")
										 : (ts == TunnelState::Starting) ? T_("TunnelStarting")
																		 : T_("StatusStopped");
		obs_properties_add_color_button_row(
			grp,
			"tunnel_controls_row",
			T_("TunnelControls"),
			tunnel_buttons,
			sizeof(tunnel_buttons) / sizeof(tunnel_buttons[0]),
			tunnel_status_dot,
			tunnel_status_text);

		std::string tunnel_domain      = extract_host_from_url(turl);
		const char *tunnel_domain_text = nullptr;
		if (cloudflared_downloading || cloudflared_dl_running) {
			tunnel_domain_text = T_("CloudflaredDownloading");
		} else if (ts == TunnelState::Starting) {
			tunnel_domain_text = T_("TunnelStarting");
		} else if (!tunnel_domain.empty()) {
			tunnel_domain_text = tunnel_domain.c_str();
		} else {
			tunnel_domain_text = T_("TunnelUnassignedDomain");
		}
		{
			obs_data_t *s = obs_source_get_settings(d->context);
			if (s) {
				obs_data_set_string(s, "tunnel_domain_info", tunnel_domain_text);
				obs_data_release(s);
			}
		}
		ods::widgets::obs_properties_add_text_readonly(
			grp,
			"tunnel_domain_info_row",
			T_("TunnelAssignedDomainLabel"),
			"tunnel_domain_info");

		if (ts == TunnelState::Running && !turl.empty()) {
			// URL 表示は「出演者別チャンネル」に集約
		} else if (ts == TunnelState::Error && !terr.empty()) {
			const std::string eb = string_printf(T_("TunnelErrorFmt"), terr.c_str());
			obs_properties_add_text(grp, "tunnel_error", eb.c_str(), OBS_TEXT_INFO);
		}
		obs_properties_add_help_callout(grp, "tunnel_domain_help", T_("TunnelDomainHelpText"));

		obs_properties_add_group(props, "grp_tunnel", T_("TunnelGroupTitle"), OBS_GROUP_NORMAL, grp);
	}

	void add_flow_group(obs_properties_t *props, DelayStreamData *d) {
		if (!props || !d) return;
		obs_properties_t *grp = obs_properties_create();
		{
			obs_property_t *ping_count_p = obs_properties_add_list(
				grp,
				"ping_count",
				T_("PingCount"),
				OBS_COMBO_TYPE_LIST,
				OBS_COMBO_FORMAT_INT);
			obs_property_list_add_int(ping_count_p, T_("PingCountDefault"), 10);
			obs_property_list_add_int(ping_count_p, T_("PingCountHigh"), 30);
			if (d->ws_any_measuring() || d->rtsp_e2e_measure.is_measuring()) {
				obs_property_set_enabled(ping_count_p, false);
			}
		}
		build_flow_panel(grp, d);
		if ((obs_get_version() >> 24) < 32) {
			obs_property_t *ver_warn_p = obs_properties_add_text(
				grp,
				"obs_version_warning",
				T_("ObsVersionWarning"),
				OBS_TEXT_INFO);
			obs_property_text_set_info_word_wrap(ver_warn_p, true);
		}
		int64_t sync_offset_ns = 0;
		if (ods::plugin::try_get_parent_audio_sync_offset_ns(d->context, sync_offset_ns) &&
			sync_offset_ns != REQUIRED_AUDIO_SYNC_OFFSET_NS) {
			obs_property_t *warn_p = obs_properties_add_text(
				grp,
				"audio_sync_offset_warning",
				T_("AudioSyncOffsetWarning"),
				OBS_TEXT_INFO);
			obs_property_text_set_info_word_wrap(warn_p, true);
		}
		obs_properties_add_group(props, "grp_flow", T_("GroupSyncFlow"), OBS_GROUP_NORMAL, grp);
	}

	void add_master_group(obs_properties_t *props, DelayStreamData *d) {
		if (!props || !d) return;
		obs_properties_t              *grp                = obs_properties_create();
		const ObsModeTextRowOptionSpec url_mode_options[] = {
			{T_("UrlModeAuto"), 1},
			{T_("UrlModeManual"), 0},
		};
		const ObsModeTextRowSpec rtmp_url_row_spec = {
			"rtmp_url_auto",
			"rtmp_url",
			url_mode_options,
			sizeof(url_mode_options) / sizeof(url_mode_options[0]),
			0,
			true,
			cb_rtmp_url_auto_changed,
			d,
			cb_rtmp_url_changed,
			d,
			true,
			0,
		};
		obs_properties_add_mode_text_row(
			grp,
			"rtmp_url_mode_row",
			T_("RtmpUrl"),
			rtmp_url_row_spec);
		const ObsModeTextRowSpec rtsp_url_row_spec = {
			ods::plugin::kRtspUseRtmpUrlKey,
			ods::plugin::kRtspUrlKey,
			url_mode_options,
			sizeof(url_mode_options) / sizeof(url_mode_options[0]),
			0,
			true,
			cb_rtsp_use_rtmp_changed,
			d,
			nullptr,
			nullptr,
			true,
			0,
		};
		obs_properties_add_mode_text_row(
			grp,
			"rtsp_url_mode_row",
			T_("RtspUrl"),
			rtsp_url_row_spec);
		std::string ffmpeg_auto_path;
		const bool  ffmpeg_auto_exists =
			ods::network::RtspE2eProber::get_auto_ffmpeg_path_if_exists(ffmpeg_auto_path);
		const bool ffmpeg_dl_running =
			d->manual_ffmpeg_download_running.load(std::memory_order_acquire);
		const ObsPathModeRowSpec ffmpeg_path_spec = {
			ods::plugin::kFfmpegExePathModeKey,
			ods::plugin::kFfmpegExePathKey,
			T_("ExePathModeAuto"),
			T_("ExePathModePath"),
			T_("ExePathModeAbsolute"),
			ffmpeg_dl_running ? T_("PathModeDownloading") : T_("PathModeDownload"),
			cb_ffmpeg_download,
			d,
			!ffmpeg_dl_running,
			true,
			0,
			ffmpeg_auto_exists,
			ffmpeg_auto_path.c_str(),
		};
		obs_properties_add_path_mode_row(
			grp,
			"ffmpeg_exe_path_row",
			T_("FfmpegExePath"),
			ffmpeg_path_spec);
		add_flow_rtsp_e2e_measure_section(grp, d);
		obs_properties_add_group(props, "grp_master", T_("GroupMasterRtmp"), OBS_GROUP_NORMAL, grp);
	}

} // namespace ods::ui
