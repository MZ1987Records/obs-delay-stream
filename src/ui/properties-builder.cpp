#include "ui/properties-builder.hpp"

#include "core/string-format.hpp"
#include "core/constants.hpp"
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
#include "widgets/mode-text-row-widget.hpp"
#include "widgets/path-mode-row-widget.hpp"
#include "widgets/pulldown-row-widget.hpp"
#include "widgets/stepper-widget.hpp"
#include "widgets/text-button-widget.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#define T_(s) obs_module_text(s)

namespace ods::ui {

	using ods::plugin::DelayStreamData;
	using ods::plugin::TabCtx;
	using ods::plugin::UpdateCheckStatus;
	using ods::sync::FlowPhase;
	using ods::sync::FlowResult;
	using ods::tunnel::TunnelState;
	using namespace ods::core;
	using namespace ods::widgets;

	namespace {
		using ods::plugin::extract_host_from_url;
		using ods::plugin::make_sub_memo_key;
		using ods::plugin::to_rtsp_url_from_rtmp;

		static constexpr int64_t REQUIRED_AUDIO_SYNC_OFFSET_NS = -950LL * 1000000LL;
		static constexpr char    kEmptyAbsolutePathSentinel[]  = "__OBS_DELAY_STREAM_EMPTY_ABSOLUTE_PATH__";

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
			d->request_props_refresh_for_tabs({1, 2}, "cb_ws_server_start");
			return false;
		}

		// WebSocket サーバーを停止し、状態表示を更新する。
		bool cb_ws_server_stop(obs_properties_t *, obs_property_t *, void *priv) {
			auto *d = static_cast<DelayStreamData *>(priv);
			if (!d || !d->router_running.load()) return false;
			d->router.stop();
			d->router_running.store(false);
			blog(LOG_INFO, "[obs-delay-stream] WebSocket server stopped");
			d->request_props_refresh_for_tabs({1, 2}, "cb_ws_server_stop");
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
			d->request_props_refresh_for_tabs({2}, "cb_tunnel_start");
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
			d->request_props_refresh_for_tabs({2}, "cb_cloudflared_download.begin");

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
						ctx->data->request_props_refresh_for_tabs({2}, "cb_cloudflared_download.done"); }, ui_ctx.release(), false);
				}).detach();
			} catch (...) {
				d->manual_cloudflared_download_running.store(false, std::memory_order_release);
				d->request_props_refresh_for_tabs({2}, "cb_cloudflared_download.spawn_error");
			}
			return false;
		}

		// cloudflared トンネルを停止する。
		bool cb_tunnel_stop(obs_properties_t *, obs_property_t *, void *priv) {
			auto *d = static_cast<DelayStreamData *>(priv);
			if (!d) return false;
			d->tunnel.stop();
			d->request_props_refresh_for_tabs({2}, "cb_tunnel_stop");
			return false;
		}

		// WebSocket 計測フローを開始する。
		bool cb_flow_start(obs_properties_t *, obs_property_t *, void *priv) {
			auto *d = static_cast<DelayStreamData *>(priv);
			if (!d) return false;
			std::string sid = d->get_stream_id();
			if (sid.empty()) return false;
			d->flow.start_ws_measurement(d->router, sid);
			return false;
		}

		// 失敗したチャンネルのみ再計測する。
		bool cb_flow_retry_failed(obs_properties_t *, obs_property_t *, void *priv) {
			auto *d = static_cast<DelayStreamData *>(priv);
			if (!d) return false;
			d->flow.retry_failed_channels(d->router);
			return false;
		}

		// RTSP E2E 計測フローを開始する。
		bool cb_flow_start_rtsp_e2e(obs_properties_t *, obs_property_t *, void *priv) {
			auto *d = static_cast<DelayStreamData *>(priv);
			if (!d || !d->context) return false;
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
			d->flow.start_rtsp_e2e_measurement(rtsp_url, ffmpeg_path, *d);
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
			d->request_props_refresh_for_tabs({4}, "cb_ffmpeg_download.begin");

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
						ctx->data->request_props_refresh_for_tabs({4}, "cb_ffmpeg_download.done"); }, ui_ctx.release(), false);
				}).detach();
			} catch (...) {
				d->manual_ffmpeg_download_running.store(false, std::memory_order_release);
				d->request_props_refresh_for_tabs({4}, "cb_ffmpeg_download.spawn_error");
			}
			return false;
		}

		// SyncFlow を中断して状態を初期化する。
		bool cb_flow_reset(obs_properties_t *, obs_property_t *, void *priv) {
			auto *d = static_cast<DelayStreamData *>(priv);
			if (!d) return false;
			d->flow.reset();
			d->inject_impulse.store(false, std::memory_order_release);
			d->request_props_refresh_for_tabs({3, 4, 5}, "cb_flow_reset");
			return false;
		}

		// stream_id の有無に応じて関連ボタンの有効状態を切り替える。
		bool cb_stream_id_changed(void *, obs_properties_t *props, obs_property_t *, obs_data_t *settings) {
			if (!props || !settings) return false;
			const char *sid     = obs_data_get_string(settings, "stream_id");
			bool        has_sid = (sid && *sid);
			if (auto *p = obs_properties_get(props, "ws_server_start_btn")) {
				obs_property_set_enabled(p, has_sid);
			}
			if (auto *p = obs_properties_get(props, "ws_server_start_note_sid")) {
				obs_property_set_visible(p, !has_sid);
			}
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
			});
			return true;
		}

		// タブ選択を設定へ反映し、プロパティを再描画する。
		bool cb_select_tab(obs_properties_t *, obs_property_t *, void *priv) {
			auto *ctx = static_cast<TabCtx *>(priv);
			if (!ctx || !ctx->d || !ctx->d->context) return false;
			ctx->d->set_active_tab(ctx->tab);
			obs_data_t *s = obs_source_get_settings(ctx->d->context);
			if (s) {
				obs_data_set_int(s, "active_tab", ctx->tab);
				obs_data_release(s);
			}
			// ColorButtonRow 側の即時再描画（return true）を使ってタブ切替遅延を減らす。
			return true;
		}

		// ============================================================
		// private 補助関数
		// ============================================================

		// RTSP E2E 計測セクションのボタン群と状態表示を構築する。
		void add_flow_rtsp_e2e_measure_section(obs_properties_t *grp, DelayStreamData *d) {
			if (!grp || !d) return;

			const FlowPhase  phase                 = d->flow.phase();
			const FlowResult res                   = d->flow.result();
			const bool       is_rtsp_e2e_measuring = (phase == FlowPhase::RtspE2eMeasuring);
			const bool       is_rtsp_e2e_done      = (phase == FlowPhase::RtspE2eDone);
			const bool       has_rtsp_e2e_result =
				(is_rtsp_e2e_done || phase == FlowPhase::Complete);
			const bool can_start_rtsp_e2e_measure =
				(phase == FlowPhase::Idle) ||
				(phase == FlowPhase::WsDone) ||
				(phase == FlowPhase::RtspE2eDone) ||
				(phase == FlowPhase::Complete);

			const ObsColorButtonSpec flow_rtsp_e2e_buttons[] = {
				{
					"flow_rtsp_e2e_measure_start_btn",
					T_("FlowMeasureStartShort"),
					cb_flow_start_rtsp_e2e,
					d,
					(!is_rtsp_e2e_measuring && can_start_rtsp_e2e_measure),
					nullptr,
					nullptr,
				},
				{
					"flow_rtsp_e2e_measure_stop_btn",
					T_("FlowMeasureStopShort"),
					cb_flow_reset,
					d,
					is_rtsp_e2e_measuring,
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
				nullptr);

			{
				int         pct = 0;
				std::string bar_text_str;
				const char *bar_text = T_("FlowNotMeasured");
				if (is_rtsp_e2e_measuring) {
					pct      = (res.rtsp_e2e_total_sets > 0)
								   ? (res.rtsp_e2e_completed_sets * 100 / res.rtsp_e2e_total_sets)
								   : 0;
					bar_text = T_("StatusMeasuring");
				} else if (has_rtsp_e2e_result && res.rtsp_e2e_valid) {
					pct          = 100;
					bar_text_str = string_printf(
						T_("RtspE2eResultFmt"),
						res.rtsp_e2e_latency_ms,
						res.rtsp_e2e_min_latency_ms,
						res.rtsp_e2e_max_latency_ms);
					bar_text = bar_text_str.c_str();
				} else if (has_rtsp_e2e_result && !res.rtsp_e2e_valid) {
					bar_text = T_("RtspE2eFailed");
				} else if (d->rtsp_e2e_measured) {
					pct          = 100;
					bar_text_str = string_printf(T_("MeasuredRtspE2eFmt"), d->measured_rtsp_e2e_ms);
					bar_text     = bar_text_str.c_str();
				}
				obs_properties_add_flow_progress(grp, "flow_s4_status", nullptr, pct, bar_text);
			}

			if (has_rtsp_e2e_result && !res.rtsp_e2e_valid && !res.rtsp_e2e_error.empty()) {
				obs_properties_add_text(
					grp,
					"flow_s4_error_detail",
					res.rtsp_e2e_error.c_str(),
					OBS_TEXT_INFO);
			}
		}

		// SyncFlow パネル全体（接続状況/操作/進捗）を構築する。
		void build_flow_panel(obs_properties_t *grp, DelayStreamData *d) {
			if (!grp || !d) return;
			obs_properties_add_text(grp, "flow_desc", T_("FlowDesc"), OBS_TEXT_INFO);

			FlowPhase  phase     = d->flow.phase();
			FlowResult res       = d->flow.result();
			int        sub_count = d->sub_ch_count;

			const bool is_complete       = (phase == FlowPhase::Complete);
			const bool is_ws_measuring   = (phase == FlowPhase::WsMeasuring);
			const bool is_ws_done        = (phase == FlowPhase::WsDone);
			const bool is_rtsp_measuring = (phase == FlowPhase::RtspE2eMeasuring);
			const bool is_rtsp_done      = (phase == FlowPhase::RtspE2eDone);
			const bool is_ws_done_or_later =
				is_ws_done || is_rtsp_measuring || is_rtsp_done || is_complete;

			obs_data_t *s               = obs_source_get_settings(d->context);
			auto        format_sub_name = [s](int ch) -> std::string {
				if (!s) return "Ch." + std::to_string(ch + 1);
				const auto  memo_key = make_sub_memo_key(ch);
				const char *memo     = obs_data_get_string(s, memo_key.data());
				if (memo && *memo) return std::string(memo);
				return "Ch." + std::to_string(ch + 1);
			};

			std::string connected_names;
			std::string disconnected_names;
			int         connected_count    = 0;
			int         disconnected_count = 0;
			for (int i = 0; i < sub_count; ++i) {
				std::string name = format_sub_name(i);
				if (d->router.client_count(i) > 0) {
					if (!connected_names.empty()) connected_names += " ";
					connected_names += name;
					++connected_count;
				} else {
					if (!disconnected_names.empty()) disconnected_names += " ";
					disconnected_names += name;
					++disconnected_count;
				}
			}

			if (connected_count == 0) connected_names = T_("FlowNone");
			if (disconnected_count == 0) disconnected_names = T_("FlowNone");

			std::string status_text = std::string(T_("FlowConnected")) + connected_names +
									  "\n" + T_("FlowDisconnected") + disconnected_names;
			obs_properties_add_text(grp, "flow_connected", status_text.c_str(), OBS_TEXT_INFO);

			const bool can_start_ws =
				(phase == FlowPhase::Idle) ||
				(phase == FlowPhase::WsDone) ||
				(phase == FlowPhase::RtspE2eDone) ||
				(phase == FlowPhase::Complete);
			const ObsColorButtonSpec flow_measure_buttons[] = {
				{
					"flow_measure_start_btn",
					T_("FlowMeasureStartShort"),
					cb_flow_start,
					d,
					(can_start_ws && connected_count > 0),
					nullptr,
					nullptr,
				},
				{
					"flow_measure_stop_btn",
					T_("FlowMeasureStopShort"),
					cb_flow_reset,
					d,
					is_ws_measuring,
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
				nullptr);

			// 保存済み計測結果の有無を判定する。
			bool has_saved_ws = false;
			if (!is_ws_measuring && !is_ws_done_or_later) {
				for (int i = 0; i < sub_count; ++i) {
					if (d->sub_channels[i].ws_measured) {
						has_saved_ws = true;
						break;
					}
				}
			}

			{
				int         pct      = 0;
				const char *bar_text = T_("FlowNotMeasured");
				if (is_ws_measuring) {
					pct      = (res.ping_total_count > 0)
								   ? res.ping_sent_count * 100 / res.ping_total_count
								   : 0;
					bar_text = T_("StatusMeasuring");
				} else if (is_ws_done_or_later || has_saved_ws) {
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

			if (is_ws_done_or_later) {
				// ライブフロー結果を表示する。
				std::string detail_text;
				for (int i = 0; i < sub_count; ++i) {
					if (!res.channels[i].connected) continue;
					const auto  memo_key = make_sub_memo_key(i);
					const char *memo     = s ? obs_data_get_string(s, memo_key.data()) : "";
					std::string name     = (memo && *memo) ? memo : ("Ch." + std::to_string(i + 1));
					if (!detail_text.empty()) detail_text += "\n";
					if (res.channels[i].measured) {
						detail_text += string_printf(
							"  Ch.%d %s : %d ms",
							i + 1,
							name.c_str(),
							res.ch_measured_ms(i));
					} else {
						detail_text +=
							"  Ch." + std::to_string(i + 1) + " " + name + " : " + T_("FlowChFailed");
					}
				}
				if (!detail_text.empty())
					obs_properties_add_text(grp, "flow_s1_detail", detail_text.c_str(), OBS_TEXT_INFO);
			} else if (has_saved_ws) {
				// 保存済み計測結果を表示する。
				std::string detail_text;
				for (int i = 0; i < sub_count; ++i) {
					if (!d->sub_channels[i].ws_measured) continue;
					const auto  memo_key = make_sub_memo_key(i);
					const char *memo     = s ? obs_data_get_string(s, memo_key.data()) : "";
					std::string name     = (memo && *memo) ? memo : ("Ch." + std::to_string(i + 1));
					if (!detail_text.empty()) detail_text += "\n";
					detail_text += string_printf(
						"  Ch.%d %s : %d ms",
						i + 1,
						name.c_str(),
						d->sub_channels[i].measured_ms);
				}
				if (!detail_text.empty())
					obs_properties_add_text(grp, "flow_s1_detail", detail_text.c_str(), OBS_TEXT_INFO);
			}

			auto *retry_failed_btn = obs_properties_add_button2(
				grp,
				"flow_retry_btn",
				T_("FlowRetryFailed"),
				cb_flow_retry_failed,
				d);
			obs_property_set_enabled(retry_failed_btn,
									 is_ws_done && (res.measured_count() < res.connected_count()));

			if (s) obs_data_release(s);
		}

	} // namespace

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
		};
		static const char *const kLocaleKeys[] = {
			"TabPerformerNames",
			"TabAudioStreaming",
			"TabUrlSharing",
			"TabSyncLatency",
			"TabRtmpLatency",
			"TabFineAdjust",
		};
		constexpr int kTabCount   = 6;
		const char   *kInactiveBg = "auto";
		if (active_tab < 0 || active_tab >= kTabCount) active_tab = 0;

		ObsColorButtonSpec buttons[kTabCount];
		for (int i = 0; i < kTabCount; ++i) {
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
			kTabCount);
	}

	void add_plugin_group(obs_properties_t *props, DelayStreamData *d) {
		if (!props || !d) return;
		obs_properties_t *grp = obs_properties_create();

		obs_property_t *about_p = obs_properties_add_text(grp, "about_info", "", OBS_TEXT_INFO);
		obs_property_set_long_description(about_p,
										  "v" PLUGIN_VERSION " | (C) 2026 Mazzn1987, Chigiri Tsutsumi | GPL 2.0+<br>"
										  "<a href=\"https://github.com/MZ1987Records/obs-delay-stream\">GitHub</a> | "
										  "<a href=\"https://mz1987records.booth.pm/items/8134637\">Booth</a>");
		obs_property_text_set_info_word_wrap(about_p, false);

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

		obs_properties_add_group(props, "grp_plugin", T_("Plugin"), OBS_GROUP_NORMAL, grp);
	}

	void add_stream_group(obs_properties_t *props, DelayStreamData *d) {
		if (!props || !d) return;
		obs_properties_t *grp = obs_properties_create();
		obs_property_t   *sid_p =
			obs_properties_add_text(grp, "stream_id", T_("StreamId"), OBS_TEXT_DEFAULT);
		obs_property_set_modified_callback2(sid_p, cb_stream_id_changed, d);
		obs_property_set_enabled(sid_p, false);
		{
			const std::string info = string_printf(T_("AutoIpFmt"), d->auto_ip.c_str());
			obs_properties_add_text(grp, "auto_ip_info", info.c_str(), OBS_TEXT_INFO);
		}
		obs_property_t *ip_p =
			obs_properties_add_text(grp, "host_ip_manual", T_("IpOverride"), OBS_TEXT_DEFAULT);
		obs_property_t *port_p =
			obs_properties_add_int(grp, "ws_port", T_("WsPort"), 1, 65535, 1);
		if (d->router_running.load()) {
			obs_property_set_enabled(sid_p, false);
			obs_property_set_enabled(ip_p, false);
			obs_property_set_enabled(port_p, false);
		}
		obs_properties_add_group(props, "grp_stream", T_("GroupStreamId"), OBS_GROUP_NORMAL, grp);
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
			true);

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
		obs_property_t *fw_note_p = obs_properties_add_text(
			grp,
			"ws_firewall_note",
			ws_firewall_note.c_str(),
			OBS_TEXT_INFO);
		obs_property_text_set_info_word_wrap(fw_note_p, false);

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
		bool              show_tunnel_start_note  = false;
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
				(!tunnel_running && !tunnel_busy && ws_running),
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

		show_tunnel_start_note = (!ws_running && !tunnel_running && !tunnel_busy);

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
		const std::string db =
			string_printf(T_("TunnelAssignedDomainFmt"), tunnel_domain_text);
		obs_properties_add_text(grp, "tunnel_domain_info", db.c_str(), OBS_TEXT_INFO);

		if (show_tunnel_start_note) {
			obs_properties_add_text(grp, "tunnel_start_note", T_("TunnelStartNote"), OBS_TEXT_INFO);
		}

		if (ts == TunnelState::Running && !turl.empty()) {
			// URL 表示は「出演者別チャンネル」に集約
		} else if (ts == TunnelState::Error && !terr.empty()) {
			const std::string eb = string_printf(T_("TunnelErrorFmt"), terr.c_str());
			obs_properties_add_text(grp, "tunnel_error", eb.c_str(), OBS_TEXT_INFO);
		}
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
			obs_property_list_add_int(ping_count_p, "10", 10);
			obs_property_list_add_int(ping_count_p, "20", 20);
			obs_property_list_add_int(ping_count_p, "30", 30);
			obs_property_list_add_int(ping_count_p, "40", 40);
			obs_property_list_add_int(ping_count_p, "50", 50);
			const FlowPhase phase = d->flow.phase();
			if (phase == FlowPhase::WsMeasuring || phase == FlowPhase::RtspE2eMeasuring) {
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
