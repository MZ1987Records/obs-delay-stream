#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "audio/audio-processor.hpp"
#include "core/constants.hpp"
#include "plugin/plugin-assets.hpp"
#include "plugin/plugin-config.hpp"
#include "plugin/plugin-helpers.hpp"
#include "plugin/plugin-services.hpp"
#include "plugin/plugin-settings.hpp"
#include "plugin/plugin-state.hpp"
#include "plugin/plugin-utils.hpp"
#include "plugin/release-check.hpp"
#include "ui/properties-builder.hpp"
#include "ui/properties-channels.hpp"
#include "ui/properties-delay.hpp"
#include "ui/properties-url-share.hpp"
#include "widgets/color-buttons-widget.hpp"
#include "widgets/delay-table-widget.hpp"
#include "widgets/flow-progress-widget.hpp"
#include "widgets/mode-text-row-widget.hpp"
#include "widgets/path-mode-row-widget.hpp"
#include "widgets/pulldown-row-widget.hpp"
#include "widgets/stepper-widget.hpp"
#include "widgets/text-button-widget.hpp"

#include <QApplication>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <obs-module.h>
#include <string>
#include <thread>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-delay-stream", "ja-JP")
#define T_(s) obs_module_text(s)

// OBSプロセス内では本フィルタを1インスタンスだけ許可する。
// bool フラグではなくソースポインタ＋世代カウンタで管理することで、
// OBS が destroy 前に create を呼ぶ "recreate パターン" を正しく扱う。
static std::mutex    g_singleton_mtx;
static obs_source_t *g_singleton_owner      = nullptr;
static uint64_t      g_singleton_generation = 0;

using ods::plugin::get_local_ip;
using ods::plugin::make_sub_measured_key;
using ods::plugin::DelayStreamData;
using ods::plugin::UpdateCheckStatus;
using ods::plugin::SubChannelCtx;
using ods::sync::FlowResult;
using ods::sync::FlowPhase;
using ods::network::LatencyResult;
using namespace ods::core;
using namespace ods::widgets;

// OBS の各コールバックを受け、DelayStreamData のライフサイクルを統括する。
class DelayStreamFilter {
public:

	static const char *get_name(void *);
	static void *create(obs_data_t *, obs_source_t *);
	static void destroy(void *);
	static void update(void *, obs_data_t *);
	static obs_audio_data *filter_audio(void *, obs_audio_data *);
	static obs_properties_t *get_properties(void *);
	static void get_defaults(obs_data_t *);

private:

	static bool cb_measure_subchannel(obs_properties_t *, obs_property_t *, void *);

	static void save_measurement_and_recalc(DelayStreamData *);
	static void queue_ui_safe(DelayStreamData *, std::function<void(DelayStreamData *)>);
	static void setup_event_callbacks(DelayStreamData *);
	static void clear_event_callbacks(DelayStreamData *);
	static void schedule_widget_injects_for_tab(DelayStreamData *, int);
	static void schedule_audio_sync_check(DelayStreamData *, std::weak_ptr<std::atomic<bool>>);
	static void start_update_check_async(DelayStreamData *, std::weak_ptr<std::atomic<bool>>);
	static void schedule_update_check(DelayStreamData *, std::weak_ptr<std::atomic<bool>>, bool);
};

// 計測結果と計測済みフラグを OBS 設定に書き戻し、全チャンネル遅延を再計算する。
void DelayStreamFilter::save_measurement_and_recalc(DelayStreamData *d) {
	obs_data_t *settings = obs_source_get_settings(d->context);
	obs_data_set_int(settings, ods::plugin::kMeasuredRtspE2eKey, d->delay.measured_rtsp_e2e_ms);
	obs_data_set_bool(settings, ods::plugin::kRtspE2eMeasuredKey, d->delay.rtsp_e2e_measured);
	for (int i = 0; i < MAX_SUB_CH; ++i) {
		const auto key = ods::plugin::make_sub_measured_key(i);
		obs_data_set_int(settings, key.data(), d->delay.channels[i].measured_ms);
		const auto ws_key = ods::plugin::make_sub_ws_measured_key(i);
		obs_data_set_bool(settings, ws_key.data(), d->delay.channels[i].ws_measured);
	}
	obs_data_release(settings);
	ods::plugin::recalc_all_delays(d);
}

// コールバックスレッドから OBS UI スレッドへ安全にディスパッチするヘルパー。
// life_token が無効なら fn は呼ばれない（use-after-free 防止）。
void DelayStreamFilter::queue_ui_safe(DelayStreamData                       *d,
									  std::function<void(DelayStreamData *)> fn) {
	// UI スレッドへ渡す最小コンテキスト。life_token で破棄後実行を防ぐ。
	struct Ctx {
		std::weak_ptr<std::atomic<bool>>       life_token;
		DelayStreamData                       *d;
		std::function<void(DelayStreamData *)> fn;
	};
	auto c = std::make_unique<Ctx>(Ctx{d->life_token, d, std::move(fn)});
	obs_queue_task(OBS_TASK_UI, [](void *p) {
		auto  c = std::unique_ptr<Ctx>(static_cast<Ctx *>(p));
		auto life = c->life_token.lock();
		if (life && life->load(std::memory_order_acquire))
					c->fn(c->d); }, c.release(), false);
}

// 表示中タブに必要なカスタムウィジェット注入だけを優先的にスケジュールする。
void DelayStreamFilter::schedule_widget_injects_for_tab(DelayStreamData *d, int active_tab) {
	if (!d || !d->context) return;
	obs_source_t *ctx = d->context;

	// タブセレクタを含む色ボタン行は常に必要。
	schedule_color_button_row_inject(ctx);

	switch (active_tab) {
	case 0:
		schedule_text_button_inject(ctx);
		break;
	case 1:
		schedule_pulldown_row_inject(ctx);
		schedule_stepper_inject(ctx);
		break;
	case 2:
		schedule_path_mode_row_inject(ctx);
		break;
	case 3:
		schedule_flow_progress_inject(ctx);
		break;
	case 4:
		schedule_flow_progress_inject(ctx);
		schedule_path_mode_row_inject(ctx);
		schedule_mode_text_row_inject(ctx);
		break;
	case 5:
		schedule_stepper_inject(ctx);
		schedule_delay_table_inject(ctx);
		break;
	default:
		break;
	}
}

// 音声同期オフセットを定期的に確認し、UIに表示している値と変化があればプロパティを再描画する。
void DelayStreamFilter::schedule_audio_sync_check(
	DelayStreamData                 *d,
	std::weak_ptr<std::atomic<bool>> life_token_weak) {
	constexpr int kIntervalMs = 3000;
	QTimer::singleShot(kIntervalMs, qApp, [d, life_token_weak]() {
		auto token = life_token_weak.lock();
		if (!token || !token->load(std::memory_order_acquire)) return;
		int64_t current = INT64_MIN;
		ods::plugin::try_get_parent_audio_sync_offset_ns(d->context, current);
		const int64_t last =
			d->last_rendered_audio_sync_offset_ns.load(std::memory_order_relaxed);
		if (current != last) {
			d->request_props_refresh("audio_sync_offset_poll");
		}
		schedule_audio_sync_check(d, life_token_weak);
	});
}

// 最新リリース確認をバックグラウンドで実行し、UIスレッドで結果を反映する。
void DelayStreamFilter::start_update_check_async(
	DelayStreamData                 *d,
	std::weak_ptr<std::atomic<bool>> life_token_weak) {
	if (!d) return;
	bool expected = false;
	if (!d->update_check.inflight.compare_exchange_strong(
			expected,
			true,
			std::memory_order_acq_rel)) {
		return;
	}
	d->update_check.status.store(UpdateCheckStatus::Checking, std::memory_order_release);

	try {
		std::thread([d, life_token_weak]() {
			ods::plugin::LatestReleaseInfo info;
			const bool                     ok = ods::plugin::fetch_latest_release_info(info);
			const bool                     has_update =
				ok && ods::plugin::is_newer_version(info.latest_version, PLUGIN_VERSION);

			// ワーカー結果を UI スレッドへ受け渡すためのコンテキスト。
			struct Ctx {
				std::weak_ptr<std::atomic<bool>> life_token;
				DelayStreamData                 *d;
				bool                             ok         = false;
				bool                             has_update = false;
				ods::plugin::LatestReleaseInfo   info;
			};
			auto ctx = std::make_unique<Ctx>(Ctx{
				life_token_weak,
				d,
				ok,
				has_update,
				std::move(info)});
			obs_queue_task(OBS_TASK_UI, [](void *p) {
				auto  ctx = std::unique_ptr<Ctx>(static_cast<Ctx *>(p));
				auto  life = ctx->life_token.lock();
				if (life && life->load(std::memory_order_acquire)) {
					DelayStreamData *d = ctx->d;
					d->update_check.set_strings(
						ctx->info.latest_version,
						ctx->info.release_url,
						ctx->info.error);
					UpdateCheckStatus new_status = UpdateCheckStatus::Error;
					if (ctx->ok) {
						new_status = ctx->has_update
										 ? UpdateCheckStatus::UpdateAvailable
										 : UpdateCheckStatus::UpToDate;
					}
					const UpdateCheckStatus prev =
						d->update_check.status.exchange(new_status, std::memory_order_acq_rel);
					d->update_check.inflight.store(false, std::memory_order_release);
					if (new_status == UpdateCheckStatus::UpdateAvailable || prev != new_status) {
						d->request_props_refresh("release_update_check");
					}
					if (ctx->ok) {
						blog(LOG_INFO,
							 "[obs-delay-stream] update check done latest=v%s current=v%s has_update=%d",
							 d->update_check.latest_version().c_str(),
							 PLUGIN_VERSION,
							 new_status == UpdateCheckStatus::UpdateAvailable ? 1 : 0);
					} else {
						blog(LOG_WARNING,
							 "[obs-delay-stream] update check failed: %s",
							 d->update_check.error().c_str());
					}
				} }, ctx.release(), false);
		}).detach();
	} catch (...) {
		d->update_check.inflight.store(false, std::memory_order_release);
		d->update_check.status.store(UpdateCheckStatus::Error, std::memory_order_release);
		d->request_props_refresh("release_update_check_spawn_error");
	}
}

// 初回起動時と一定間隔で更新確認を行う。
void DelayStreamFilter::schedule_update_check(
	DelayStreamData                 *d,
	std::weak_ptr<std::atomic<bool>> life_token_weak,
	bool                             immediate) {
	constexpr int kInitialDelayMs = 1200;
	constexpr int kIntervalMs     = 6 * 60 * 60 * 1000;
	const int     delay           = immediate ? kInitialDelayMs : kIntervalMs;
	QTimer::singleShot(delay, qApp, [d, life_token_weak]() {
		auto token = life_token_weak.lock();
		if (!token || !token->load(std::memory_order_acquire)) return;
		start_update_check_async(d, life_token_weak);
		schedule_update_check(d, life_token_weak, false);
	});
}

const char *DelayStreamFilter::get_name(void *) {
	return "obs-delay-stream";
}

// 各コンポーネントのイベントコールバックを登録する。
void DelayStreamFilter::setup_event_callbacks(DelayStreamData *d) {
	d->flow.on_update = [d]() {
		d->request_props_refresh_for_tabs({3, 4}, "flow.on_update");
	};
	d->flow.on_progress = [d]() {
		const FlowResult res   = d->flow.result();
		const FlowPhase  phase = d->flow.phase();
		int              pct   = 0;
		if (phase == FlowPhase::WsMeasuring) {
			pct = res.ping_total_count > 0
					  ? res.ping_sent_count * 100 / res.ping_total_count
					  : 0;
		} else if (phase == FlowPhase::RtspE2eMeasuring || phase == FlowPhase::RtspE2eDone) {
			pct = res.rtsp_e2e_total_sets > 0
					  ? res.rtsp_e2e_completed_sets * 100 / res.rtsp_e2e_total_sets
					  : 0;
		}
		update_flow_progress(d->context, pct);
	};
	d->flow.on_ch_measured = [d](int, LatencyResult) {
		d->request_props_refresh_for_tabs({3}, "flow.on_ch_measured");
	};
	d->flow.on_rtsp_e2e_measured = [d](int rtsp_e2e_ms) {
		queue_ui_safe(d, [rtsp_e2e_ms](DelayStreamData *d) {
			d->delay.measured_rtsp_e2e_ms = rtsp_e2e_ms;
			d->delay.rtsp_e2e_measured    = true;
			save_measurement_and_recalc(d);
			d->request_props_refresh_for_tabs({4, 5}, "flow.on_rtsp_e2e_measured");
		});
	};
	d->flow.on_ws_measured = [d](const FlowResult &res) {
		queue_ui_safe(d, [res](DelayStreamData *d) {
			for (int i = 0; i < MAX_SUB_CH; ++i) {
				if (!res.channels[i].measured) continue;
				d->delay.channels[i].measured_ms = res.ch_measured_ms(i);
				d->delay.channels[i].ws_measured = true;
			}
			save_measurement_and_recalc(d);
			d->request_props_refresh_for_tabs({3, 5}, "flow.on_ws_measured");
		});
	};
	d->tunnel.on_url_ready = [d](const std::string &) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		d->request_props_refresh_for_tabs({2}, "tunnel.on_url_ready");
	};
	d->tunnel.on_error = [d](const std::string &) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		d->request_props_refresh_for_tabs({2}, "tunnel.on_error");
	};
	d->tunnel.on_stopped = [d]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		d->request_props_refresh_for_tabs({2}, "tunnel.on_stopped");
	};
	d->tunnel.on_download_state = [d](bool downloading) {
		if (!downloading) {
			queue_ui_safe(d, [](DelayStreamData *d) {
				ods::plugin::maybe_persist_cloudflared_path_after_auto_ready(d->context);
				d->request_props_refresh_for_tabs({2}, "tunnel.on_download_state.done");
			});
			return;
		}
		d->request_props_refresh_for_tabs({2}, "tunnel.on_download_state");
	};
	d->router.on_conn_change = [d](const std::string &sid, int, size_t) {
		if (sid == d->get_stream_id()) {
			d->request_props_refresh_for_tabs({3}, "router.on_conn_change");
		}
	};
	d->router.on_any_latency_result = [d](const std::string &sid, int ch, LatencyResult r) {
		if (sid != d->get_stream_id()) return;
		if (ch < 0 || ch >= MAX_SUB_CH) return;
		if (d->flow.phase() != FlowPhase::Idle) return;
		d->sub_channels[ch].measure.set_result(r, r.valid ? "" : T_("MeasureFailed"));
		if (r.valid) {
			queue_ui_safe(d, [ch, ms_val = static_cast<int>(std::lround(r.avg_latency_ms))](DelayStreamData *d) {
				d->delay.channels[ch].measured_ms = ms_val;
				d->delay.channels[ch].ws_measured = true;
				save_measurement_and_recalc(d);
				d->request_props_refresh_for_tabs({5}, "router.on_any_latency_result.apply");
			});
		} else {
			d->request_props_refresh_for_tabs({5}, "router.on_any_latency_result.invalid");
		}
	};
}

// 全コンポーネントのイベントコールバックを解除する。
void DelayStreamFilter::clear_event_callbacks(DelayStreamData *d) {
	d->flow.on_update            = nullptr;
	d->flow.on_progress          = nullptr;
	d->flow.on_ch_measured       = nullptr;
	d->flow.on_rtsp_e2e_measured = nullptr;
	d->flow.on_ws_measured       = nullptr;
	d->tunnel.on_url_ready       = nullptr;
	d->tunnel.on_error           = nullptr;
	d->tunnel.on_stopped         = nullptr;
	d->tunnel.on_download_state  = nullptr;
	d->router.clear_callbacks();
}

// フィルタインスタンスを生成して各コンポーネントを初期化する。
void *DelayStreamFilter::create(obs_data_t *settings, obs_source_t *source) {
	blog(LOG_INFO, "[obs-delay-stream] create START source=%s", obs_source_get_name(source));
	auto *d = new DelayStreamData();
	blog(LOG_INFO, "[obs-delay-stream] create: DelayStreamData allocated at %p", (void *)d);
	d->context = source;
	{
		std::lock_guard<std::mutex> lk(g_singleton_mtx);
		if (g_singleton_owner == nullptr || g_singleton_owner == source) {
			g_singleton_owner        = source;
			d->singleton_generation  = ++g_singleton_generation;
			d->is_duplicate_instance = false;
			d->owns_singleton_slot   = true;
		} else {
			blog(LOG_WARNING,
				 "[obs-delay-stream] create: slot already owned by source=%s; "
				 "marking new instance (source=%s) as duplicate",
				 obs_source_get_name(g_singleton_owner),
				 obs_source_get_name(source));
			d->is_duplicate_instance = true;
			d->owns_singleton_slot   = false;
		}
	}
	ods::ui::props_refresh_unblock_source(source);
	ods::plugin::maybe_autofill_rtmp_url(settings, false);
	d->rtmp_url_auto.store(obs_data_get_bool(settings, "rtmp_url_auto"), std::memory_order_relaxed);
	if (d->is_duplicate_instance) {
		blog(LOG_WARNING, "[obs-delay-stream] duplicate filter instance created as warning-only");
		d->create_done.store(true);
		return d;
	}
	{
		auto html = ods::plugin::load_receiver_index_html();
		if (html.empty()) {
			blog(LOG_WARNING, "[obs-delay-stream] receiver/index.html not found; HTTP top page disabled");
		} else {
			blog(LOG_INFO, "[obs-delay-stream] receiver html loaded: build=%s", ods::plugin::get_receiver_build_timestamp());
		}
		d->router.set_http_index_html(std::move(html));
		auto root = ods::plugin::get_receiver_root_dir();
		if (!root.empty()) d->router.set_http_root_dir(std::move(root));
	}
	d->auto_ip = get_local_ip();
	d->host_ip = d->auto_ip;
	for (int i = 0; i < MAX_SUB_CH; ++i) {
		d->sub_btn_ctx[i] = {d, i};
	}
	for (int i = 0; i < 6; ++i) {
		d->tab_btn_ctx[i] = {d, i};
	}
	setup_event_callbacks(d);

	update(d, settings);
	d->create_done.store(true);
	queue_ui_safe(d, [](DelayStreamData *dp) {
		auto life = std::weak_ptr<std::atomic<bool>>(dp->life_token);
		schedule_audio_sync_check(dp, life);
		schedule_update_check(dp, life, true);
	});
	blog(LOG_INFO, "[obs-delay-stream] create complete");
	return d;
}

// フィルタインスタンス破棄時に各コンポーネントを安全に停止する。
void DelayStreamFilter::destroy(void *data) {
	auto d = std::unique_ptr<DelayStreamData>(static_cast<DelayStreamData *>(data));
	if (!d) return;
	d->destroying.store(true, std::memory_order_release);
	if (d->life_token) {
		d->life_token->store(false, std::memory_order_release);
		d->life_token.reset();
	}
	if (d->context) {
		ods::ui::props_refresh_block_source(d->context);
		flow_progress_unregister_source(d->context);
	}
	bool          release_singleton_slot = d->owns_singleton_slot;
	obs_source_t *my_source              = d->context;
	uint64_t      my_gen                 = d->singleton_generation;
	if (!d->is_duplicate_instance) {
		d->flow.reset();
		d->tunnel.stop();
		d->router.stop();
		clear_event_callbacks(d.get());
	}
	d.reset();
	if (release_singleton_slot) {
		std::lock_guard<std::mutex> lk(g_singleton_mtx);
		if (g_singleton_owner == my_source && g_singleton_generation == my_gen) {
			g_singleton_owner = nullptr;
		}
	}
}

void DelayStreamFilter::update(void *data, obs_data_t *settings) {
	ods::plugin::apply_settings(static_cast<DelayStreamData *>(data), settings);
}

obs_audio_data *DelayStreamFilter::filter_audio(void *data, obs_audio_data *audio) {
	return ods::audio::filter_audio_delay_stream(
		static_cast<DelayStreamData *>(data),
		audio);
}

bool DelayStreamFilter::cb_measure_subchannel(obs_properties_t *, obs_property_t *, void *priv) {
	auto *ctx = static_cast<SubChannelCtx *>(priv);
	auto *d   = ctx->d;
	int   i   = ctx->ch;
	if (d->get_stream_id().empty() || d->sub_channels[i].measure.is_measuring()) return false;
	if (d->router.client_count(i) == 0) return false;
	bool ok = d->router.start_measurement(i, d->ping_count_setting.load(std::memory_order_relaxed), PING_INTV_MS);
	if (ok) d->sub_channels[i].measure.start();
	d->request_props_refresh_for_tabs({3, 5}, "cb_measure_subchannel");
	return false;
}

obs_properties_t *DelayStreamFilter::get_properties(void *data) {
	obs_properties_t *props = obs_properties_create();
	if (!data) return props;
	auto *d = static_cast<DelayStreamData *>(data);
	// get_properties の再入を検知し、戻りで深さを必ず復元する。
	struct GetPropsDepthGuard {
		DelayStreamData *d;
		~GetPropsDepthGuard() {
			if (d) d->get_props_depth.fetch_sub(1, std::memory_order_acq_rel);
		}
	};
	int                prev_depth = d->get_props_depth.fetch_add(1, std::memory_order_acq_rel);
	GetPropsDepthGuard depth_guard{d};
	if (prev_depth > 0) {
		blog(LOG_INFO, "[obs-delay-stream] get_properties re-entry depth=%d", prev_depth + 1);
	}

	bool has_sid = !d->get_stream_id().empty();

	{
		int64_t sync_offset_ns = INT64_MIN;
		ods::plugin::try_get_parent_audio_sync_offset_ns(d->context, sync_offset_ns);
		d->last_rendered_audio_sync_offset_ns.store(sync_offset_ns, std::memory_order_relaxed);
	}

	int active_tab = d->get_active_tab();
	ods::ui::add_plugin_group(props, d);
	if (!d->is_duplicate_instance) {
		ods::ui::add_tab_selector_row(props, d, active_tab);

		switch (active_tab) {
		case 0:
			ods::ui::channels::add_sub_channels_group(props, d);
			break;
		case 1:
			ods::ui::add_ws_group(props, d, has_sid);
			ods::ui::add_stream_group(props, d);
			break;
		case 2:
			ods::ui::add_tunnel_group(props, d);
			ods::ui::url_share::add_url_share_group(props, d);
			break;
		case 3:
			ods::ui::add_flow_group(props, d);
			break;
		case 4:
			ods::ui::add_master_group(props, d);
			break;
		case 5:
			ods::ui::delay::add_avatar_latency_group(props, d);
			ods::ui::delay::add_delay_summary_group(props, d);
			break;
		default:
			break;
		}
	}

	if (!d->is_duplicate_instance) {
		schedule_widget_injects_for_tab(d, active_tab);
	}

	return props;
}

void DelayStreamFilter::get_defaults(obs_data_t *settings) {
	ods::plugin::set_delay_stream_defaults(settings);
}

static struct obs_source_info delay_stream_filter;

static void register_source_info() {
	memset(&delay_stream_filter, 0, sizeof(delay_stream_filter));
	delay_stream_filter.id             = "delay_stream_filter";
	delay_stream_filter.type           = OBS_SOURCE_TYPE_FILTER;
	delay_stream_filter.output_flags   = OBS_SOURCE_AUDIO;
	delay_stream_filter.get_name       = DelayStreamFilter::get_name;
	delay_stream_filter.create         = DelayStreamFilter::create;
	delay_stream_filter.destroy        = DelayStreamFilter::destroy;
	delay_stream_filter.update         = DelayStreamFilter::update;
	delay_stream_filter.filter_audio   = DelayStreamFilter::filter_audio;
	delay_stream_filter.get_properties = DelayStreamFilter::get_properties;
	delay_stream_filter.get_defaults   = DelayStreamFilter::get_defaults;
}

bool obs_module_load(void) {
	register_source_info();
	obs_register_source(&delay_stream_filter);
	blog(LOG_INFO, "[obs-delay-stream] v" PLUGIN_VERSION " loaded");
	return true;
}
// 明示的な後処理は不要のため空実装。
void obs_module_unload(void) {}
