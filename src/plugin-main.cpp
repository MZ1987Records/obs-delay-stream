/*
 * obs-delay-stream
 * Clean rewrite
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <stdint.h>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <algorithm>
#include <chrono>
#include <memory>
#include <QApplication>
#include <QTimer>
#include <obs-module.h>
#include "constants.hpp"
#include "plugin-main-utils.hpp"
#include "plugin-main-obs-services.hpp"
#include "plugin-main-props-refresh.hpp"
#include "plugin-main-receiver-assets.hpp"
#include "plugin-main-config.hpp"
#include "plugin-main-settings-helpers.hpp"
#include "plugin-main-state.hpp"
#include "plugin-main-sub-settings.hpp"
#include "plugin-main-audio-processing.hpp"
#include "plugin-main-update.hpp"
#include "plugin-main-sub-channels-ui.hpp"
#include "plugin-main-delay-ui.hpp"
#include "plugin-main-url-share-ui.hpp"
#include "plugin-main-properties-ui.hpp"
#include "stepper-widget.hpp"
#include "text-button-widget.hpp"
#include "color-buttons-widget.hpp"
#include "delay-table-widget.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-delay-stream", "ja-JP")
#define T_(s) obs_module_text(s)

// OBSプロセス内では本フィルタを1インスタンスだけ許可する。
// bool フラグではなくソースポインタ＋世代カウンタで管理することで、
// OBS が destroy 前に create を呼ぶ "recreate パターン" を正しく扱う。
static std::mutex        g_singleton_mtx;
static obs_source_t*     g_singleton_owner      = nullptr;
static uint64_t          g_singleton_generation = 0;

using plugin_main_utils::get_local_ip;
using plugin_main_sub_settings::make_sub_delay_key;

// プロパティUIの再描画を依頼する。
static void request_properties_refresh(DelayStreamData* d, const char* reason = nullptr) {
    if (!d) return;
    plugin_main_props_refresh::props_refresh_request(
        d->context,
        d->create_done.load(std::memory_order_acquire),
        d->destroying.load(std::memory_order_acquire),
        d->get_props_depth.load(std::memory_order_acquire),
        reason);
}

// マスター遅延を設定へ書き戻し、必要ならバッファへ反映する。
static void write_master_delay(DelayStreamData* d, double ms) {
    obs_data_t* s = obs_source_get_settings(d->context);
    obs_data_set_double(s, "master_delay_ms", ms);
    d->master_delay_ms = (float)ms;
    if (d->enabled.load()) d->master_buf.set_delay_ms((uint32_t)ms);
    obs_data_release(s);
}

// コールバックスレッドから OBS UI スレッドへ安全にディスパッチするヘルパー。
// life_token が無効なら fn は呼ばれない（use-after-free 防止）。
static void queue_ui_safe(DelayStreamData* d,
                           std::function<void(DelayStreamData*)> fn)
{
    struct Ctx {
        std::weak_ptr<std::atomic<bool>> life_token;
        DelayStreamData* d;
        std::function<void(DelayStreamData*)> fn;
    };
    auto* c = new Ctx{d->life_token, d, std::move(fn)};
    obs_queue_task(OBS_TASK_UI, [](void* p) {
        auto* c = static_cast<Ctx*>(p);
        auto life = c->life_token.lock();
        if (life && life->load(std::memory_order_acquire))
            c->fn(c->d);
        delete c;
    }, c, false);
}

// Forward declarations
static const char*       ds_get_name(void*);
static void*             ds_create(obs_data_t*, obs_source_t*);
static void              ds_destroy(void*);
static void              ds_update(void*, obs_data_t*);
static obs_audio_data*   ds_filter_audio(void*, obs_audio_data*);
static obs_properties_t* ds_get_properties(void*);
static void              ds_get_defaults(obs_data_t*);
static void              apply_sub_delay(DelayStreamData*, int, double);
static void              setup_event_callbacks(DelayStreamData*);
static void              clear_event_callbacks(DelayStreamData*);

static bool cb_sub_measure(obs_properties_t*, obs_property_t*, void*);

// 音声同期オフセットを定期的に確認し、UIに表示している値と変化があればプロパティを再描画する。
// QTimer::singleShot で自己再スケジュールする（低頻度: 3 秒ごと）。
// life_token が無効になると再スケジュールを止め、自然に停止する。
static void schedule_audio_sync_check(
    DelayStreamData* d,
    std::weak_ptr<std::atomic<bool>> life_token_weak)
{
    constexpr int kIntervalMs = 3000;
    QTimer::singleShot(kIntervalMs, qApp, [d, life_token_weak]() {
        auto token = life_token_weak.lock();
        if (!token || !token->load(std::memory_order_acquire)) return;
        int64_t current = INT64_MIN;
        plugin_main_properties_ui::try_get_parent_audio_sync_offset_ns(d, current);
        const int64_t last =
            d->last_rendered_audio_sync_offset_ns.load(std::memory_order_relaxed);
        if (current != last) {
            request_properties_refresh(d, "audio_sync_offset_poll");
        }
        schedule_audio_sync_check(d, life_token_weak);
    });
}

static struct obs_source_info delay_stream_filter;

// OBSへ登録するソース情報テーブルを初期化する。
static void register_source_info() {
    memset(&delay_stream_filter, 0, sizeof(delay_stream_filter));
    delay_stream_filter.id             = "delay_stream_filter";
    delay_stream_filter.type           = OBS_SOURCE_TYPE_FILTER;
    delay_stream_filter.output_flags   = OBS_SOURCE_AUDIO;
    delay_stream_filter.get_name       = ds_get_name;
    delay_stream_filter.create         = ds_create;
    delay_stream_filter.destroy        = ds_destroy;
    delay_stream_filter.update         = ds_update;
    delay_stream_filter.filter_audio   = ds_filter_audio;
    delay_stream_filter.get_properties = ds_get_properties;
    delay_stream_filter.get_defaults   = ds_get_defaults;
}

// プラグイン読み込み時にフィルタを登録する。
bool obs_module_load(void) {
    register_source_info();
    obs_register_source(&delay_stream_filter);
    blog(LOG_INFO, "[obs-delay-stream] v" PLUGIN_VERSION " loaded");
    return true;
}
// 明示的な後処理は不要のため空実装。
void obs_module_unload(void) {}

// OBS表示名を返す。
static const char* ds_get_name(void*) { return "obs-delay-stream"; }

// 各コンポーネントのイベントコールバックを登録する。
// コールバックは対応するコンポーネントのワーカースレッドから呼ばれるため、
// UI 操作が必要な処理は queue_ui_safe() 経由で UI スレッドへ移譲する。
static void setup_event_callbacks(DelayStreamData* d) {
    d->flow.on_update      = [d]() { request_properties_refresh(d, "flow.on_update"); };
    d->flow.on_ch_measured = [d](int, LatencyResult) {
        request_properties_refresh(d, "flow.on_ch_measured");
    };
    d->flow.on_apply_master = [d](double ms) {
        queue_ui_safe(d, [ms](DelayStreamData* d) {
            write_master_delay(d, ms);
            request_properties_refresh(d, "flow.on_apply_master");
        });
    };
    d->flow.on_apply_sub_delays = [d](const FlowResult& res) {
        queue_ui_safe(d, [res](DelayStreamData* d) {
            for (int i = 0; i < MAX_SUB_CH; ++i) {
                const auto& ch = res.channels[i];
                if (!ch.measured) continue;
                apply_sub_delay(d, i, ch.proposed_delay);
            }
            request_properties_refresh(d, "flow.on_apply_sub_delays");
        });
    };
    d->rtmp.prober.on_result = [d](RtmpProbeResult r) {
        { std::lock_guard<std::mutex> lk(d->rtmp.mtx);
          d->rtmp.result = r; d->rtmp.applied = false; }
        request_properties_refresh(d, "rtmp.prober.on_result");
    };
    // Tunnel callbacks - delay 100ms to ensure properties view is ready
    d->tunnel.on_url_ready = [d](const std::string&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        request_properties_refresh(d, "tunnel.on_url_ready");
    };
    d->tunnel.on_error = [d](const std::string&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        request_properties_refresh(d, "tunnel.on_error");
    };
    d->tunnel.on_stopped = [d]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        request_properties_refresh(d, "tunnel.on_stopped");
    };
    d->tunnel.on_download_state = [d](bool downloading) {
        if (!downloading) {
            queue_ui_safe(d, [](DelayStreamData* d) {
                plugin_main_settings_helpers::maybe_persist_cloudflared_path_after_auto_ready(d->context);
                request_properties_refresh(d, "tunnel.on_download_state.done");
            });
            return;
        }
        request_properties_refresh(d, "tunnel.on_download_state");
    };
    d->router.on_conn_change = [d](const std::string& sid, int, size_t) {
        if (sid == d->get_stream_id()) request_properties_refresh(d, "router.on_conn_change");
    };
    d->router.on_any_latency_result = [d](const std::string& sid, int ch, LatencyResult r) {
        if (sid != d->get_stream_id()) return;
        if (ch < 0 || ch >= MAX_SUB_CH) return;
        // SyncFlow 実行中または完了後は proposed_delay が権威。raw latency で上書きしない。
        if (d->flow.phase() != FlowPhase::Idle) return;
        auto& ms = d->sub[ch].measure;
        bool should_apply = r.valid;
        {
            std::lock_guard<std::mutex> lk(ms.mtx);
            ms.result    = r;
            ms.measuring = false;
            ms.applied   = should_apply;
            ms.last_error = r.valid ? "" : T_("MeasureFailed");
        }
        if (should_apply) {
            queue_ui_safe(d, [ch, ms_val = r.avg_one_way](DelayStreamData* d) {
                apply_sub_delay(d, ch, ms_val);
                request_properties_refresh(d, "router.on_any_latency_result.apply");
            });
        } else {
            request_properties_refresh(d, "router.on_any_latency_result.invalid");
        }
    };
}

// 全コンポーネントのイベントコールバックを解除する。
// コンポーネント停止後に呼ぶこと（競合なし）。
static void clear_event_callbacks(DelayStreamData* d) {
    d->flow.on_update          = nullptr;
    d->flow.on_ch_measured     = nullptr;
    d->flow.on_apply_master    = nullptr;
    d->flow.on_apply_sub_delays = nullptr;
    d->rtmp.prober.on_result = nullptr;
    d->tunnel.on_url_ready   = nullptr;
    d->tunnel.on_error       = nullptr;
    d->tunnel.on_stopped     = nullptr;
    d->tunnel.on_download_state = nullptr;
    d->router.clear_callbacks();
}

// フィルタインスタンスを生成して各コンポーネントを初期化する。
static void* ds_create(obs_data_t* settings, obs_source_t* source) {
    blog(LOG_INFO, "[obs-delay-stream] ds_create START source=%s",
         obs_source_get_name(source));
    auto* d = new DelayStreamData();
    blog(LOG_INFO, "[obs-delay-stream] ds_create: DelayStreamData allocated at %p", (void*)d);
    d->context  = source;
    // 同一ソースの recreate（destroy 前に create が呼ばれる OBS のパターン）は
    // duplicate 扱いしない。世代カウンタで古い destroy がスロットを誤解放しないよう守る。
    {
        std::lock_guard<std::mutex> lk(g_singleton_mtx);
        if (g_singleton_owner == nullptr || g_singleton_owner == source) {
            g_singleton_owner         = source;
            d->singleton_generation   = ++g_singleton_generation;
            d->is_duplicate_instance  = false;
            d->owns_singleton_slot    = true;
        } else {
            blog(LOG_WARNING,
                 "[obs-delay-stream] ds_create: slot already owned by source=%s; "
                 "marking new instance (source=%s) as duplicate",
                 obs_source_get_name(g_singleton_owner),
                 obs_source_get_name(source));
            d->is_duplicate_instance = true;
            d->owns_singleton_slot   = false;
        }
    }
    plugin_main_props_refresh::props_refresh_unblock_source(source);
    plugin_main_obs_services::maybe_autofill_rtmp_url(settings, false);
    d->rtmp_url_auto.store(obs_data_get_bool(settings, "rtmp_url_auto"), std::memory_order_relaxed);
    if (d->is_duplicate_instance) {
        blog(LOG_WARNING, "[obs-delay-stream] duplicate filter instance created as warning-only");
        d->create_done.store(true);
        return d;
    }
    {
        auto html = plugin_main_receiver_assets::load_receiver_index_html();
        if (html.empty()) {
            blog(LOG_WARNING, "[obs-delay-stream] receiver/index.html not found; HTTP top page disabled");
        } else {
            blog(LOG_INFO, "[obs-delay-stream] receiver html loaded: build=%s",
                 plugin_main_receiver_assets::get_receiver_build_timestamp());
        }
        d->router.set_http_index_html(std::move(html));
        auto root = plugin_main_receiver_assets::get_receiver_root_dir();
        if (!root.empty()) d->router.set_http_root_dir(std::move(root));
    }
    d->auto_ip  = get_local_ip();
    d->host_ip  = d->auto_ip;
    for (int i = 0; i < MAX_SUB_CH; ++i) {
        d->btn_ctx[i]         = { d, i };
    }
    setup_event_callbacks(d);

    ds_update(d, settings);
    d->create_done.store(true);
    // 音声同期オフセット変化の定期チェックを UI スレッドで開始する。
    queue_ui_safe(d, [](DelayStreamData* dp) {
        schedule_audio_sync_check(dp, std::weak_ptr<std::atomic<bool>>(dp->life_token));
    });
    blog(LOG_INFO, "[obs-delay-stream] ds_create complete");
    return d;
}

// フィルタインスタンス破棄時に各コンポーネントを安全に停止する。
static void ds_destroy(void* data) {
    auto* d = static_cast<DelayStreamData*>(data);
    if (!d) return;
    d->destroying.store(true, std::memory_order_release);
    if (d->life_token) {
        d->life_token->store(false, std::memory_order_release);
        d->life_token.reset();
    }
    if (d->context) {
        plugin_main_props_refresh::props_refresh_block_source(d->context);
    }
    bool        release_singleton_slot = d->owns_singleton_slot;
    obs_source_t* my_source            = d->context;
    uint64_t      my_gen               = d->singleton_generation;
    // 1. 各コンポーネントの停止（内部スレッドの join を含む）
    if (!d->is_duplicate_instance) {
        d->flow.reset();
        d->rtmp.prober.cancel();
        d->tunnel.stop();
        d->router.stop();

        // 2. コールバックをnull化（停止後なので競合しない）
        clear_event_callbacks(d);
    }

    delete d;
    if (release_singleton_slot) {
        // 世代が一致するときだけ解放する。
        // recreate パターンで新インスタンスがすでにスロットを引き継いでいる場合は
        // 世代が進んでいるため、古いインスタンスの destroy がスロットを誤解放しない。
        std::lock_guard<std::mutex> lk(g_singleton_mtx);
        if (g_singleton_owner == my_source && g_singleton_generation == my_gen) {
            g_singleton_owner = nullptr;
        }
    }
}

// OBS設定値を内部状態へ同期する。
static void ds_update(void* data, obs_data_t* settings) {
    plugin_main_update::apply_settings(static_cast<DelayStreamData*>(data), settings);
}

// マスター遅延適用とチャンネル配信用の音声分岐を行う。
static obs_audio_data* ds_filter_audio(void* data, obs_audio_data* audio) {
    return plugin_main_audio_processing::filter_audio_delay_stream(static_cast<DelayStreamData*>(data), audio);
}

// Button callbacks
// 選択されたチャンネルの往復遅延測定を開始する。
static bool cb_sub_measure(obs_properties_t*, obs_property_t*, void* priv) {
    auto* ctx = static_cast<ChCtx*>(priv);
    auto* d = ctx->d; int i = ctx->ch;
    if (d->get_stream_id().empty() || d->sub[i].measure.measuring) return false;
    if (d->router.client_count(i) == 0) return false;
    bool ok = d->router.start_measurement(i, d->ping_count_setting.load(std::memory_order_relaxed), PING_INTV_MS);
    if (ok) {
        auto& ms = d->sub[i].measure;
        std::lock_guard<std::mutex> lk(ms.mtx);
        ms.measuring = true;
        ms.result = LatencyResult{};
        ms.last_error.clear();
    }
    request_properties_refresh(d, "cb_sub_measure");
    return false;
}
// 指定チャンネルの遅延を設定値と実バッファへ反映する。
static void apply_sub_delay(DelayStreamData* d, int i, double ms) {
    if (!d || i < 0 || i >= MAX_SUB_CH) return;
    obs_data_t* s = obs_source_get_settings(d->context);
    const auto delay_key = make_sub_delay_key(i);
    obs_data_set_double(s, delay_key.data(), ms);
    obs_data_release(s);
    d->sub[i].delay_ms = (float)ms;
    plugin_main_audio_processing::apply_sub_delay_to_buffer(d, i);
    d->router.notify_apply_delay(
        i,
        d->sub[i].delay_ms,
        "auto_measure");
}

// OBSプロパティパネル全体を構築する。
static obs_properties_t* ds_get_properties(void* data) {
    obs_properties_t* props = obs_properties_create();
    if (!data) return props;
    auto* d = static_cast<DelayStreamData*>(data);
    struct GetPropsDepthGuard {
        DelayStreamData* d;
        ~GetPropsDepthGuard() {
            if (d) {
                d->get_props_depth.fetch_sub(1, std::memory_order_acq_rel);
            }
        }
    };
    int prev_depth = d->get_props_depth.fetch_add(1, std::memory_order_acq_rel);
    GetPropsDepthGuard depth_guard{d};
    if (prev_depth > 0) {
        blog(LOG_INFO, "[obs-delay-stream] ds_get_properties re-entry depth=%d",
            prev_depth + 1);
    }

    bool has_sid = !d->get_stream_id().empty();

    // ポーリング差分検出のために描画時の同期オフセットを記録する。
    {
        int64_t sync_offset_ns = INT64_MIN;
        plugin_main_properties_ui::try_get_parent_audio_sync_offset_ns(d, sync_offset_ns);
        d->last_rendered_audio_sync_offset_ns.store(sync_offset_ns, std::memory_order_relaxed);
    }

    // UIブロックを順に組み立てる
    plugin_main_properties_ui::add_plugin_group(props, d);
    if (d->is_duplicate_instance) {
        return props;
    }
    plugin_main_sub_channels_ui::add_sub_channels_group(props, d);
    plugin_main_properties_ui::add_stream_group(props, d);
    plugin_main_properties_ui::add_ws_group(props, d, has_sid);
    plugin_main_properties_ui::add_tunnel_group(props, d);
    plugin_main_url_share_ui::add_url_share_group(props, d);
    plugin_main_properties_ui::add_flow_group(props, d);
    plugin_main_properties_ui::add_master_group(props, d);
    plugin_main_delay_ui::add_sub_offset_group(props, d);
    plugin_main_delay_ui::add_delay_summary_group(props, d);

    // ステッパー注入をスケジュール（UIスレッドでプレースホルダーを差し替え）
    if (d->context) {
        schedule_stepper_inject(d->context);
        schedule_text_button_inject(d->context);
        schedule_color_button_row_inject(d->context);
        schedule_delay_table_inject(d->context);
    }

    return props;
}

// 各設定項目のデフォルト値を定義する。
static void ds_get_defaults(obs_data_t* settings) {
    plugin_main_config::set_delay_stream_defaults(settings);
}
