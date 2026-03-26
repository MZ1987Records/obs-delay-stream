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
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#include <stdint.h>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <array>
#include <map>
#include <set>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <memory>
#include <random>
#include <obs-module.h>
#include <util/platform.h>
#include "delay-filter.hpp"
#include "receiver_index_html.hpp"
#include "websocket-server.hpp"
#include "rtmp-prober.hpp"
#include "sync-flow.hpp"
#include "tunnel-manager.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-delay-stream", "ja-JP")
#define T_(s) obs_module_text(s)

static constexpr int   NUM_SUB_CH      = 10;
static constexpr int   WS_PORT         = 19000;
static constexpr float MAX_DELAY_MS    = 10000.0f;
static constexpr int   PING_COUNT      = 10;
static constexpr int   PING_INTV_MS    = 150;
static constexpr int   RTMP_PROBE_CNT  = 10;
static constexpr int   RTMP_PROBE_INTV = 300;

// ローカルネットワークで利用しやすいIPv4アドレスを優先して取得する。
static std::string get_local_ip() {
    ULONG buf_size = 15000;
    std::vector<BYTE> buf(buf_size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_INFO*>(buf.data());
    if (GetAdaptersInfo(adapters, &buf_size) != NO_ERROR) return "127.0.0.1";
    std::string fallback;
    for (auto* a = adapters; a; a = a->Next) {
        for (auto* ip = &a->IpAddressList; ip; ip = ip->Next) {
            std::string addr = ip->IpAddress.String;
            if (addr == "0.0.0.0" || addr.empty()) continue;
            if (addr.rfind("192.168.", 0) == 0 || addr.rfind("10.", 0) == 0) return addr;
            unsigned a0,a1,a2,a3;
            if (sscanf(addr.c_str(), "%u.%u.%u.%u", &a0,&a1,&a2,&a3) == 4
                && a0 == 172 && a1 >= 16 && a1 <= 31) return addr;
            if (fallback.empty()) fallback = addr;
        }
    }
    return fallback.empty() ? "127.0.0.1" : fallback;
}

// UTF-8文字列をクリップボードへUNICODEテキストとしてコピーする。
static void copy_to_clipboard(const std::string& text) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    std::wstring w;
    if (!text.empty()) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                       static_cast<int>(text.size()),
                                       nullptr, 0);
        if (wlen > 0) {
            w.resize(static_cast<size_t>(wlen));
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                static_cast<int>(text.size()),
                                &w[0], wlen);
        }
    }
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
    if (h) {
        wchar_t* p = static_cast<wchar_t*>(GlobalLock(h));
        if (p) {
            if (!w.empty()) memcpy(p, w.data(), w.size() * sizeof(wchar_t));
            p[w.size()] = L'\0';
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        }
    }
    CloseClipboard();
}

// 配信用IDに使える文字のみを残して正規化する。
static std::string sanitize_stream_id(const char* raw) {
    if (!raw) return "";
    std::string s(raw);
    s.erase(std::remove_if(s.begin(), s.end(), [](char c){
        return !std::isalnum((unsigned char)c) && c != '-' && c != '_';
    }), s.end());
    return s;
}

// ランダムな配信用IDを生成する。
static std::string generate_stream_id(size_t len = 12) {
    static const char kChars[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<size_t> dist(0, sizeof(kChars) - 2);
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) out.push_back(kChars[dist(rng)]);
    return out;
}

// Sub-CH数を有効範囲(1..NUM_SUB_CH)に丸める。
static int clamp_sub_ch_count(int v) {
    if (v < 1) return 1;
    if (v > NUM_SUB_CH) return NUM_SUB_CH;
    return v;
}

// ファイル全体を文字列として読み込む。
static std::string read_file_to_string(const char* path) {
    if (!path || !*path) return "";
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return "";
    return std::string((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());
}

// receiver/index.html をモジュール配下→相対パス→埋め込みの順で読み込む。
static std::string load_receiver_index_html() {
    std::string html;
    char* mod_path = obs_module_file("receiver/index.html");
    if (mod_path) {
        html = read_file_to_string(mod_path);
        bfree(mod_path);
    }
    if (html.empty()) {
        html = read_file_to_string("receiver/index.html");
    }
    if (html.empty()) {
        html = std::string(kReceiverIndexHtml);
    }
    return html;
}

// receiver/index.html の配置ディレクトリを返す。
static std::string get_receiver_root_dir() {
    std::string path;
    char* mod_path = obs_module_file("receiver/index.html");
    if (mod_path) {
        path = mod_path;
        bfree(mod_path);
    }
    if (path.empty()) {
        path = "receiver/index.html";
    }
    auto pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return "";
    return path.substr(0, pos);
}

// OBS本体からRTMP URLを取得する拡張ポイント（現状は未使用）。
static std::string get_obs_stream_url() {
    return ""; // Set RTMP URL manually in the plugin panel
}

struct MeasureState {
    std::mutex    mtx;
    LatencyResult result;
    bool          measuring = false;
    bool          applied   = false;
    std::string   last_error;
};

struct RtmpMeasureState {
    RtmpProber      prober;
    std::mutex      mtx;
    RtmpProbeResult result;
    bool            applied = false;
    std::string     cached_url;
};

struct DelayStreamData;

struct ChCtx { DelayStreamData* d; int ch; };

struct DelayStreamData {
    obs_source_t* context      = nullptr;
    std::atomic<bool> enabled{true};
    std::atomic<bool> ws_send_enabled{true};
    mutable std::mutex stream_id_mtx;  // protects stream_id, host_ip
    std::string   stream_id;
    std::string   host_ip;
    std::string   auto_ip;
    float         master_delay_ms = 0.0f;
    float         sub_offset_ms   = 0.0f; // global offset added to all sub-ch after Sync Flow
    int           sub_ch_count    = 1; // active sub-channel count (1..NUM_SUB_CH)
    DelayBuffer   master_buf;
    RtmpMeasureState rtmp;
    StreamRouter  router;
    std::atomic<bool> router_running{false};
    std::array<ChCtx, NUM_SUB_CH> btn_ctx;
    struct SubChannel {
        float        delay_ms = 0.0f;
        DelayBuffer  buf;
        MeasureState measure;
    };
    std::array<SubChannel, NUM_SUB_CH> sub;
    SyncFlow      flow;
    TunnelManager tunnel;
    uint32_t      sample_rate = 48000;
    uint32_t      channels    = 2;
    bool          initialized = false;
    std::atomic<bool> create_done{false}; // set true after ds_create completes
    std::atomic<bool> in_get_props{false}; // true while ds_get_properties is running
    std::atomic<bool> sid_autofill_guard{false};
    std::vector<float> work_buf;

    // Thread-safe accessors for stream_id
    // 現在の配信IDをスレッドセーフに取得する。
    std::string get_stream_id() const {
        std::lock_guard<std::mutex> lk(stream_id_mtx);
        return stream_id;
    }
    // 配信IDをスレッドセーフに更新する。
    void set_stream_id(const std::string& id) {
        std::lock_guard<std::mutex> lk(stream_id_mtx);
        stream_id = id;
    }
    // 現在の配信用ホストIPをスレッドセーフに取得する。
    std::string get_host_ip() const {
        std::lock_guard<std::mutex> lk(stream_id_mtx);
        return host_ip;
    }
};

// Sub-CHの最終遅延値（base + global offset）を算出して、無効時は0にする。
static uint32_t calc_effective_sub_delay_ms(const DelayStreamData* d, float base_delay_ms) {
    if (!d || !d->enabled.load(std::memory_order_relaxed)) return 0;
    float effective = base_delay_ms + d->sub_offset_ms;
    if (effective < 0.0f) effective = 0.0f;
    return static_cast<uint32_t>(effective);
}

// 指定Sub-CHのバッファ遅延を現在設定へ反映する。
static void apply_sub_delay_to_buffer(DelayStreamData* d, int ch) {
    if (!d || ch < 0 || ch >= NUM_SUB_CH) return;
    d->sub[ch].buf.set_delay_ms(calc_effective_sub_delay_ms(d, d->sub[ch].delay_ms));
}

// 測定状態を初期値へ戻す。
static void clear_measure_state(MeasureState& ms) {
    std::lock_guard<std::mutex> lk(ms.mtx);
    ms.result = LatencyResult{};
    ms.measuring = false;
    ms.applied = false;
    ms.last_error.clear();
}

// OBS自動取得値が空の場合は、UI設定の手入力URLを使う。
static std::string resolve_rtmp_url(DelayStreamData* d) {
    if (!d || !d->context) return "";
    std::string url = get_obs_stream_url();
    if (!url.empty()) return url;
    obs_data_t* s = obs_source_get_settings(d->context);
    if (!s) return "";
    const char* manual = obs_data_get_string(s, "rtmp_url_manual");
    if (manual && *manual) url = manual;
    obs_data_release(s);
    return url;
}

// cloudflaredの自動検出結果があれば設定へ反映する。
static void maybe_fill_cloudflared_path_from_auto(DelayStreamData* d) {
    if (!d || !d->context) return;
    obs_data_t* s = obs_source_get_settings(d->context);
    const char* cur = obs_data_get_string(s, "cloudflared_exe_path");
    bool is_auto = (!cur || !*cur || _stricmp(cur, "auto") == 0);
    if (is_auto) {
        std::string auto_path;
        if (TunnelManager::get_auto_cloudflared_path_if_exists(auto_path)) {
            std::string ui_path = TunnelManager::to_localappdata_env_path(auto_path);
            if (!cur || _stricmp(cur, ui_path.c_str()) != 0) {
                obs_data_set_string(s, "cloudflared_exe_path", ui_path.c_str());
            }
        }
    }
    obs_data_release(s);
}

// プロパティUIの再描画をUIスレッドへ依頼する。
static void request_properties_refresh(DelayStreamData* d) {
    if (!d || !d->context || !d->create_done.load()) return;
    if (d->in_get_props.load()) return; // prevent re-entry
    obs_source_t* ctx = d->context;
    obs_queue_task(OBS_TASK_UI, [](void* p) {
        obs_source_update_properties(static_cast<obs_source_t*>(p));
    }, ctx, false);
}

// Sync FlowのStep2結果をSub-CH遅延設定へ書き戻す。
static void write_sub_delays(DelayStreamData* d, const FlowResult& r) {
    obs_data_t* s = obs_source_get_settings(d->context);
    for (int i = 0; i < NUM_SUB_CH; ++i) {
        if (!r.channels[i].measured) continue;
        char key[32]; snprintf(key, sizeof(key), "sub%d_delay_ms", i);
        obs_data_set_double(s, key, r.channels[i].proposed_delay);
        d->sub[i].delay_ms = (float)r.channels[i].proposed_delay;
        d->sub[i].buf.set_delay_ms((uint32_t)r.channels[i].proposed_delay);
        d->router.notify_apply_delay(i, r.channels[i].proposed_delay);
    }
    obs_data_release(s);
    // Note: ds_update will be called by OBS when settings change
}

// マスター遅延を設定へ書き戻し、必要ならバッファへ反映する。
static void write_master_delay(DelayStreamData* d, double ms) {
    obs_data_t* s = obs_source_get_settings(d->context);
    obs_data_set_double(s, "master_delay_ms", ms);
    d->master_delay_ms = (float)ms;
    if (d->enabled.load()) d->master_buf.set_delay_ms((uint32_t)ms);
    obs_data_release(s);
}

// 指定チャンネル用の受信用URLを生成する。
static std::string make_sub_url(DelayStreamData* d, int ch1) {
    std::string sid = d->get_stream_id();
    if (sid.empty()) return "";
    std::string base;
    std::string turl = d->tunnel.url();
    if (!turl.empty()) {
        base = turl;
        if (base.rfind("wss://", 0) == 0) base.replace(0, 6, "https://");
        else if (base.rfind("ws://", 0) == 0) base.replace(0, 5, "http://");
    } else {
        std::string ip = d->get_host_ip();
        if (ip.empty()) ip = d->auto_ip;
        if (ip.empty()) return "";
        char buf[256];
        snprintf(buf, sizeof(buf), "http://%s:%d", ip.c_str(), WS_PORT);
        base = buf;
    }
    if (!base.empty() && base.back() == '/') base.pop_back();
    char buf[512];
    snprintf(buf, sizeof(buf), "%s/#!/%s/%d", base.c_str(), sid.c_str(), ch1);
    return buf;
}


// Forward declarations
static const char*       ds_get_name(void*);
static void*             ds_create(obs_data_t*, obs_source_t*);
static void              ds_destroy(void*);
static void              ds_update(void*, obs_data_t*);
static obs_audio_data*   ds_filter_audio(void*, obs_audio_data*);
static obs_properties_t* ds_get_properties(void*);
static void              ds_get_defaults(obs_data_t*);
static void              ensure_init(DelayStreamData*, uint32_t, uint32_t);
static void              build_flow_panel(obs_properties_t*, DelayStreamData*);
static void              apply_sub_delay(DelayStreamData*, int, double);

static bool cb_sub_measure(obs_properties_t*, obs_property_t*, void*);
static bool cb_sub_apply(obs_properties_t*, obs_property_t*, void*);
static bool cb_sub_copy_all(obs_properties_t*, obs_property_t*, void*);
static bool cb_sub_add(obs_properties_t*, obs_property_t*, void*);
static bool cb_sub_remove(obs_properties_t*, obs_property_t*, void*);
static bool cb_rtmp_measure(obs_properties_t*, obs_property_t*, void*);
static bool cb_rtmp_apply(obs_properties_t*, obs_property_t*, void*);
static bool cb_tunnel_start(obs_properties_t*, obs_property_t*, void*);
static bool cb_tunnel_stop(obs_properties_t*, obs_property_t*, void*);
static bool cb_flow_start(obs_properties_t*, obs_property_t*, void*);
static bool cb_flow_retry_failed(obs_properties_t*, obs_property_t*, void*);
static bool cb_flow_apply_step2(obs_properties_t*, obs_property_t*, void*);
static bool cb_flow_start_step3(obs_properties_t*, obs_property_t*, void*);
static bool cb_flow_apply_step3(obs_properties_t*, obs_property_t*, void*);
static bool cb_flow_reset(obs_properties_t*, obs_property_t*, void*);
static bool cb_enabled_changed(void*, obs_properties_t*, obs_property_t*, obs_data_t*);
static bool cb_stream_id_changed(void*, obs_properties_t*, obs_property_t*, obs_data_t*);
static bool cb_host_ip_changed(void*, obs_properties_t*, obs_property_t*, obs_data_t*);
static bool cb_detail_mode_changed(void*, obs_properties_t*, obs_property_t*, obs_data_t*);
static bool cb_ws_server_start(obs_properties_t*, obs_property_t*, void*);
static bool cb_ws_server_stop(obs_properties_t*, obs_property_t*, void*);

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

// フィルタインスタンスを生成して各コンポーネントを初期化する。
static void* ds_create(obs_data_t* settings, obs_source_t* source) {
    blog(LOG_INFO, "[obs-delay-stream] ds_create START");
    auto* d = new DelayStreamData();
    blog(LOG_INFO, "[obs-delay-stream] ds_create: DelayStreamData allocated at %p", (void*)d);
    d->context  = source;
    {
        auto html = load_receiver_index_html();
        if (html.empty()) {
            blog(LOG_WARNING, "[obs-delay-stream] receiver/index.html not found; HTTP top page disabled");
        }
        d->router.set_http_index_html(std::move(html));
        auto root = get_receiver_root_dir();
        if (!root.empty()) d->router.set_http_root_dir(std::move(root));
    }
    d->auto_ip  = get_local_ip();
    d->host_ip  = d->auto_ip;
    for (int i = 0; i < NUM_SUB_CH; ++i) {
        d->btn_ctx[i]         = { d, i };
    }
    d->flow.on_update      = [d]() { request_properties_refresh(d); };
    d->flow.on_ch_measured = [d](int, LatencyResult) { request_properties_refresh(d); };
    d->flow.on_apply_sub = [d](const FlowResult& r) {
        struct Ctx { DelayStreamData* d; FlowResult r; };
        auto* c = new Ctx{d, r};
        obs_queue_task(OBS_TASK_UI, [](void* p){
            auto* c = static_cast<Ctx*>(p);
            write_sub_delays(c->d, c->r);
            request_properties_refresh(c->d);
            delete c;
        }, c, false);
    };
    d->flow.on_apply_master = [d](double ms) {
        struct Ctx { DelayStreamData* d; double ms; };
        auto* c = new Ctx{d, ms};
        obs_queue_task(OBS_TASK_UI, [](void* p){
            auto* c = static_cast<Ctx*>(p);
            write_master_delay(c->d, c->ms);
            request_properties_refresh(c->d);
            delete c;
        }, c, false);
    };
    d->rtmp.prober.on_result = [d](RtmpProbeResult r) {
        { std::lock_guard<std::mutex> lk(d->rtmp.mtx);
          d->rtmp.result = r; d->rtmp.applied = false; }
        request_properties_refresh(d);
    };
    // Tunnel callbacks - delay 100ms to ensure properties view is ready
    d->tunnel.on_url_ready = [d](const std::string&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        request_properties_refresh(d);
    };
    d->tunnel.on_error = [d](const std::string&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        request_properties_refresh(d);
    };
    d->tunnel.on_stopped = [d]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        request_properties_refresh(d);
    };
    d->tunnel.on_download_state = [d](bool) {
        request_properties_refresh(d);
    };
    d->router.on_conn_change = [d](const std::string& sid, int, size_t) {
        if (sid == d->get_stream_id()) request_properties_refresh(d);
    };
    d->router.on_any_latency_result = [d](const std::string& sid, int ch, LatencyResult r) {
        if (sid != d->get_stream_id()) return;
        if (ch < 0 || ch >= NUM_SUB_CH) return;
        auto& ms = d->sub[ch].measure;
        bool was_measuring = false;
        {
            std::lock_guard<std::mutex> lk(ms.mtx);
            was_measuring = ms.measuring;
            ms.result = r;
            ms.measuring = false;
            ms.applied = false;
            ms.last_error = r.valid ? "" : T_("MeasureFailed");
        }
        if (r.valid && was_measuring) {
            struct Ctx { DelayStreamData* d; int ch; double ms; };
            auto* c = new Ctx{d, ch, r.avg_one_way};
            obs_queue_task(OBS_TASK_UI, [](void* p){
                auto* c = static_cast<Ctx*>(p);
                apply_sub_delay(c->d, c->ch, c->ms);
                request_properties_refresh(c->d);
                delete c;
            }, c, false);
        } else {
            request_properties_refresh(d);
        }
    };

    ds_update(d, settings);
    d->create_done.store(true);
    blog(LOG_INFO, "[obs-delay-stream] ds_create complete");
    return d;
}

// フィルタインスタンス破棄時に各コンポーネントを安全に停止する。
static void ds_destroy(void* data) {
    auto* d = static_cast<DelayStreamData*>(data);
    // 1. 各コンポーネントの停止（内部スレッドの join を含む）
    d->flow.reset();
    d->rtmp.prober.cancel();
    d->tunnel.stop();
    d->router.stop();

    // 2. コールバックをnull化（停止後なので競合しない）
    d->flow.on_update       = nullptr;
    d->flow.on_ch_measured  = nullptr;
    d->flow.on_apply_sub    = nullptr;
    d->flow.on_apply_master = nullptr;
    d->rtmp.prober.on_result = nullptr;
    d->tunnel.on_url_ready  = nullptr;
    d->tunnel.on_error      = nullptr;
    d->tunnel.on_stopped    = nullptr;
    d->tunnel.on_download_state = nullptr;
    d->router.clear_callbacks();

    delete d;
}

// サンプルレート/チャンネル変更時に内部バッファを再初期化する。
static void ensure_init(DelayStreamData* d, uint32_t sr, uint32_t ch) {
    if (d->initialized && d->sample_rate == sr && d->channels == ch) return;
    d->sample_rate = sr; d->channels = ch;
    d->master_buf.init(sr, ch);
    d->master_buf.set_delay_ms((uint32_t)d->master_delay_ms);
    for (int i = 0; i < NUM_SUB_CH; ++i) {
        d->sub[i].buf.init(sr, ch);
        d->sub[i].buf.set_delay_ms((uint32_t)d->sub[i].delay_ms);
    }
    d->work_buf.resize(65536 * ch, 0.0f);
    d->initialized = true;
}

// OBS設定値を内部状態へ同期する。
static void ds_update(void* data, obs_data_t* settings) {
    auto* d = static_cast<DelayStreamData*>(data);
    bool delay_disable = obs_data_get_bool(settings, "delay_disable");
    if (!obs_data_has_user_value(settings, "delay_disable")) {
        bool legacy_enabled = obs_data_get_bool(settings, "enabled");
        delay_disable = !legacy_enabled;
        obs_data_set_bool(settings, "delay_disable", delay_disable);
    }
    d->enabled.store(!delay_disable);
    bool paused = obs_data_get_bool(settings, "ws_send_paused");
    if (!obs_data_has_user_value(settings, "ws_send_paused")) {
        bool ws_send = obs_data_get_bool(settings, "ws_send_enabled");
        if (!obs_data_has_user_value(settings, "ws_send_enabled")) {
            ws_send = obs_data_get_bool(settings, "ws_enabled");
        }
        paused = !ws_send;
        obs_data_set_bool(settings, "ws_send_paused", paused);
    }
    d->ws_send_enabled.store(!paused);
    {
        int v = (int)obs_data_get_int(settings, "sub_ch_count");
        if (v <= 0) v = 1;
        int clamped = clamp_sub_ch_count(v);
        if (clamped != v) obs_data_set_int(settings, "sub_ch_count", clamped);
        bool changed = (clamped != d->sub_ch_count);
        d->sub_ch_count = clamped;
        d->router.set_active_channels(clamped);
        d->flow.set_active_channels(clamped);
        if (changed) d->flow.reset();
    }
    int audio_codec = (int)obs_data_get_int(settings, "audio_codec");
    d->router.set_audio_codec(audio_codec);
    const char* raw = obs_data_get_string(settings, "stream_id");
    std::string sid = raw ? sanitize_stream_id(raw) : "";
    if (sid.empty()) {
        if (!d->sid_autofill_guard.exchange(true)) {
            sid = generate_stream_id(12);
            obs_data_set_string(settings, "stream_id", sid.c_str());
            d->sid_autofill_guard.store(false);
        } else {
            // 再入中は現状の値を使う
            sid = raw ? sanitize_stream_id(raw) : "";
        }
    }
    d->set_stream_id(sid);
    d->router.set_stream_id(sid);
    const char* hip = obs_data_get_string(settings, "host_ip_manual");
    {
        std::lock_guard<std::mutex> lk(d->stream_id_mtx);
        d->host_ip = (hip && *hip) ? hip : d->auto_ip;
    }
    d->master_delay_ms = (float)obs_data_get_double(settings, "master_delay_ms");
    d->master_buf.set_delay_ms(d->enabled.load() ? (uint32_t)d->master_delay_ms : 0);
    d->sub_offset_ms = (float)obs_data_get_double(settings, "sub_offset_ms");
    for (int i = 0; i < NUM_SUB_CH; ++i) {
        char key[32]; snprintf(key, sizeof(key), "sub%d_delay_ms", i);
        d->sub[i].delay_ms = (float)obs_data_get_double(settings, key);
        char memo_key[32]; snprintf(memo_key, sizeof(memo_key), "sub%d_memo", i);
        const char* memo = obs_data_get_string(settings, memo_key);
        d->router.set_sub_memo(i, memo ? memo : "");
        apply_sub_delay_to_buffer(d, i);
    }
}

// マスター遅延適用とSub-CH配信用の音声分岐を行う。
static obs_audio_data* ds_filter_audio(void* data, obs_audio_data* audio) {
    auto* d = static_cast<DelayStreamData*>(data);
    if (!audio || audio->frames == 0) return audio;
    const audio_output_info* info = audio_output_get_info(obs_get_audio());
    uint32_t sr = info ? info->samples_per_sec : 48000;
    uint32_t ch = (uint32_t)audio_output_get_channels(obs_get_audio());
    if (ch < 1) ch = 2;
    ensure_init(d, sr, ch);
    size_t frames = audio->frames;
    size_t total  = frames * ch;
    if (d->work_buf.size() < total * 3) d->work_buf.resize(total * 3, 0.0f);
    float* in  = d->work_buf.data();
    float* out = d->work_buf.data() + total;
    float* sub = d->work_buf.data() + total * 2;
    for (uint32_t c = 0; c < ch; ++c) {
        if (!audio->data[c]) continue;
        const float* src = reinterpret_cast<const float*>(audio->data[c]);
        for (size_t f = 0; f < frames; ++f) in[f*ch+c] = src[f];
    }
    bool en = d->enabled.load(std::memory_order_relaxed);
    bool ws = d->ws_send_enabled.load(std::memory_order_relaxed);
    bool rr = d->router_running.load(std::memory_order_relaxed);
    bool has_sid = !d->get_stream_id().empty();
    int sub_count = d->sub_ch_count;

    if (en) {
        d->master_buf.process(in, out, frames);
        for (uint32_t c = 0; c < ch; ++c) {
            if (!audio->data[c]) continue;
            float* dst = reinterpret_cast<float*>(audio->data[c]);
            for (size_t f = 0; f < frames; ++f) dst[f] = out[f*ch+c];
        }
        if (ws && rr && has_sid) {
            for (int i = 0; i < sub_count; ++i) {
                d->sub[i].buf.process(in, sub, frames);
                d->router.send_audio(i, sub, frames, sr, ch);
            }
        }
    } else {
        if (ws && rr && has_sid)
            for (int i = 0; i < sub_count; ++i)
                d->router.send_audio(i, in, frames, sr, ch);
    }
    return audio;
}

// Button callbacks
// 選択されたSub-CHの往復遅延測定を開始する。
static bool cb_sub_measure(obs_properties_t*, obs_property_t*, void* priv) {
    auto* ctx = static_cast<ChCtx*>(priv);
    auto* d = ctx->d; int i = ctx->ch;
    if (d->get_stream_id().empty() || d->sub[i].measure.measuring) return false;
    if (d->router.client_count(i) == 0) return false;
    bool ok = d->router.start_measurement(i, PING_COUNT, PING_INTV_MS);
    if (ok) {
        auto& ms = d->sub[i].measure;
        std::lock_guard<std::mutex> lk(ms.mtx);
        ms.measuring = true;
        ms.result = LatencyResult{};
        ms.last_error.clear();
    }
    request_properties_refresh(d);
    return false;
}
// 指定Sub-CHの遅延を設定値と実バッファへ反映する。
static void apply_sub_delay(DelayStreamData* d, int i, double ms) {
    if (!d || i < 0 || i >= NUM_SUB_CH) return;
    obs_data_t* s = obs_source_get_settings(d->context);
    char key[32]; snprintf(key, sizeof(key), "sub%d_delay_ms", i);
    obs_data_set_double(s, key, ms);
    obs_data_release(s);
    d->sub[i].delay_ms = (float)ms;
    apply_sub_delay_to_buffer(d, i);
    d->router.notify_apply_delay(i, ms);
}
// 測定結果の片道遅延をSub-CHへ適用する。
static bool cb_sub_apply(obs_properties_t*, obs_property_t*, void* priv) {
    auto* ctx = static_cast<ChCtx*>(priv);
    auto* d = ctx->d; int i = ctx->ch;
    LatencyResult r;
    { std::lock_guard<std::mutex> lk(d->sub[i].measure.mtx); r = d->sub[i].measure.result; }
    if (!r.valid) return false;
    apply_sub_delay(d, i, r.avg_one_way);
    request_properties_refresh(d);
    return false;
}
// 全Sub-CHのURLとメモをタブ区切りでクリップボードへコピーする。
static bool cb_sub_copy_all(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    if (!d) return false;
    obs_data_t* s = obs_source_get_settings(d->context);
    std::string out;
    out.reserve(512);
    int sub_count = d->sub_ch_count;
    for (int i = 0; i < sub_count; ++i) {
        std::string url = make_sub_url(d, i + 1);
        char memo_key[32]; snprintf(memo_key, sizeof(memo_key), "sub%d_memo", i);
        const char* memo = obs_data_get_string(s, memo_key);
        const char* url_show = url.empty() ? T_("NotConfigured") : url.c_str();
        if (!out.empty()) out += "\r\n";
        out += "Ch.";
        out += std::to_string(i + 1);
        out += "\t";
        if (memo && *memo) out += memo;
        out += "\t";
        out += url_show;
    }
    obs_data_release(s);
    if (!out.empty()) copy_to_clipboard(out);
    return false;
}
// Sub-CHを1つ追加する。
static bool cb_sub_add(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    if (!d) return false;
    if (d->router_running.load()) return false;
    int cur = d->sub_ch_count;
    if (cur >= NUM_SUB_CH) return false;
    int next = clamp_sub_ch_count(cur + 1);
    obs_data_t* s = obs_source_get_settings(d->context);
    obs_data_set_int(s, "sub_ch_count", next);
    obs_data_release(s);
    d->sub_ch_count = next;
    d->router.set_active_channels(next);
    d->flow.set_active_channels(next);
    d->flow.reset();
    request_properties_refresh(d);
    return false;
}
// 指定Sub-CHを削除し、後続チャンネルを前詰めする。
static bool cb_sub_remove(obs_properties_t*, obs_property_t*, void* priv) {
    auto* ctx = static_cast<ChCtx*>(priv);
    if (!ctx || !ctx->d) return false;
    auto* d = ctx->d;
    if (d->router_running.load()) return false;
    int cur = d->sub_ch_count;
    if (cur <= 1) return false;
    int ch = ctx->ch;
    if (ch < 0 || ch >= cur) return false;
    int next = clamp_sub_ch_count(cur - 1);
    obs_data_t* s = obs_source_get_settings(d->context);
    // shift settings down: ch+1 -> ch (including inactive tail)
    for (int i = ch; i < NUM_SUB_CH - 1; ++i) {
        char key_from[32], key_to[32];
        snprintf(key_from, sizeof(key_from), "sub%d_delay_ms", i + 1);
        snprintf(key_to,   sizeof(key_to),   "sub%d_delay_ms", i);
        double v = obs_data_get_double(s, key_from);
        obs_data_set_double(s, key_to, v);

        char memo_from[32], memo_to[32];
        snprintf(memo_from, sizeof(memo_from), "sub%d_memo", i + 1);
        snprintf(memo_to,   sizeof(memo_to),   "sub%d_memo", i);
        const char* m = obs_data_get_string(s, memo_from);
        obs_data_set_string(s, memo_to, m ? m : "");
        d->router.set_sub_memo(i, m ? m : "");
    }
    // clear last
    {
        char key_last[32]; snprintf(key_last, sizeof(key_last), "sub%d_delay_ms", NUM_SUB_CH - 1);
        obs_data_set_double(s, key_last, 0.0);
        char memo_last[32]; snprintf(memo_last, sizeof(memo_last), "sub%d_memo", NUM_SUB_CH - 1);
        obs_data_set_string(s, memo_last, "");
        d->router.set_sub_memo(NUM_SUB_CH - 1, "");
    }
    obs_data_set_int(s, "sub_ch_count", next);
    obs_data_release(s);

    // shift runtime state down
    for (int i = ch; i < NUM_SUB_CH - 1; ++i) {
        d->sub[i].delay_ms = d->sub[i + 1].delay_ms;
        apply_sub_delay_to_buffer(d, i);
        clear_measure_state(d->sub[i].measure);
    }
    d->sub[NUM_SUB_CH - 1].delay_ms = 0.0f;
    apply_sub_delay_to_buffer(d, NUM_SUB_CH - 1);
    clear_measure_state(d->sub[NUM_SUB_CH - 1].measure);

    d->sub_ch_count = next;
    d->router.set_active_channels(next);
    d->flow.set_active_channels(next);
    d->flow.reset();
    request_properties_refresh(d);
    return false;
}
// RTMP遅延測定を開始する。
static bool cb_rtmp_measure(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    if (d->rtmp.prober.is_running()) return false;
    std::string url = resolve_rtmp_url(d);
    if (url.empty()) return false;
    { std::lock_guard<std::mutex> lk(d->rtmp.mtx); d->rtmp.cached_url = url; }
    d->rtmp.prober.start(url, RTMP_PROBE_CNT, RTMP_PROBE_INTV);
    return false;
}
// RTMP測定結果をマスター遅延へ適用する。
static bool cb_rtmp_apply(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    RtmpProbeResult r;
    { std::lock_guard<std::mutex> lk(d->rtmp.mtx); r = d->rtmp.result; }
    if (!r.valid) return false;
    write_master_delay(d, r.avg_one_way);
    request_properties_refresh(d);
    return false;
}
// WebSocketサーバーを起動する。
static bool cb_ws_server_start(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    if (d->router_running.load()) return false;
    if (d->router.start(WS_PORT)) {
        d->router_running.store(true);
        blog(LOG_INFO, "[obs-delay-stream] WebSocket server started on port %d", WS_PORT);
    } else {
        blog(LOG_ERROR, "[obs-delay-stream] WebSocket server FAILED to start on port %d", WS_PORT);
    }
    request_properties_refresh(d);
    return false;
}

// WebSocketサーバーを停止する。
static bool cb_ws_server_stop(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    if (!d->router_running.load()) return false;
    d->router.stop();
    d->router_running.store(false);
    blog(LOG_INFO, "[obs-delay-stream] WebSocket server stopped");
    request_properties_refresh(d);
    return false;
}

// 選択されたトンネルサービスを起動する。
static bool cb_tunnel_start(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    obs_data_t* s = obs_source_get_settings(d->context);
    int type = (int)obs_data_get_int(s, "tunnel_type");
    const char* exe = type == 0
        ? obs_data_get_string(s, "ngrok_exe_path")
        : obs_data_get_string(s, "cloudflared_exe_path");
    const char* tok = obs_data_get_string(s, "ngrok_token");
    obs_data_release(s);
    d->tunnel.start(type == 0 ? TunnelType::Ngrok : TunnelType::Cloudflared,
        exe ? exe : "", tok ? tok : "", WS_PORT);
    request_properties_refresh(d);
    return false;
}
// トンネル種別に応じて入力項目の表示/非表示を切り替える。
static bool cb_tunnel_type_changed(obs_properties_t* props, obs_property_t*, obs_data_t* settings) {
    int type = (int)obs_data_get_int(settings, "tunnel_type");
    bool is_ngrok = (type == 0);
    if (auto* p = obs_properties_get(props, "ngrok_exe_path")) {
        obs_property_set_visible(p, is_ngrok);
    }
    if (auto* p = obs_properties_get(props, "ngrok_token")) {
        obs_property_set_visible(p, is_ngrok);
    }
    if (auto* p = obs_properties_get(props, "cloudflared_exe_path")) {
        obs_property_set_visible(p, !is_ngrok);
    }
    return true;
}
// トンネルを停止する。
static bool cb_tunnel_stop(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    d->tunnel.stop();
    request_properties_refresh(d);
    return false;
}
// Sync Flow Step1（Sub-CH測定）を開始する。
static bool cb_flow_start(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    std::string sid = d->get_stream_id();
    if (sid.empty()) return false;
    d->flow.start_step1(d->router, sid);
    return false;
}
// Step1の失敗チャンネルのみ再測定する。
static bool cb_flow_retry_failed(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    d->flow.retry_failed_step1(d->router);
    return false;
}
// Step2結果を適用する。
static bool cb_flow_apply_step2(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    d->flow.apply_step2();
    request_properties_refresh(d);
    return false;
}
// Sync Flow Step3（RTMP測定）を開始する。
static bool cb_flow_start_step3(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    std::string url = resolve_rtmp_url(d);
    d->flow.start_step3(url);
    return false;
}
// Step3結果をマスター遅延へ適用する。
static bool cb_flow_apply_step3(obs_properties_t*, obs_property_t*, void* priv) {
    static_cast<DelayStreamData*>(priv)->flow.apply_step3();
    return false;
}
// Sync Flow状態を初期化する。
static bool cb_flow_reset(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    d->flow.reset();
    request_properties_refresh(d);
    return false;
}
// 遅延有効/無効を切り替え、各バッファへ反映する。
static bool cb_enabled_changed(void* priv, obs_properties_t*, obs_property_t*, obs_data_t* settings) {
    auto* d = static_cast<DelayStreamData*>(priv);
    if (!d) return false;
    bool delay_disable = obs_data_get_bool(settings, "delay_disable");
    bool en = !delay_disable;
    d->enabled.store(en);
    d->master_buf.set_delay_ms(en ? (uint32_t)d->master_delay_ms : 0);
    for (int i = 0; i < NUM_SUB_CH; ++i)
        apply_sub_delay_to_buffer(d, i);
    request_properties_refresh(d);
    return false;
}
// Stream IDの有無に応じて起動ボタン状態を更新する。
static bool cb_stream_id_changed(void* priv, obs_properties_t* props, obs_property_t*, obs_data_t* settings) {
    (void)priv;
    if (!props || !settings) return false;
    const char* sid = obs_data_get_string(settings, "stream_id");
    bool has_sid = (sid && *sid);
    if (auto* p = obs_properties_get(props, "ws_server_start_btn")) {
        obs_property_set_enabled(p, has_sid);
    }
    if (auto* p = obs_properties_get(props, "ws_server_start_note_sid")) {
        obs_property_set_visible(p, !has_sid);
    }
    return true;
}
// host_ip_manual変更時の将来拡張用フック。
static bool cb_host_ip_changed(void* priv, obs_properties_t*, obs_property_t*, obs_data_t*) {
    (void)priv; return false;
}
// 詳細表示モードに応じて各詳細項目の可視性を切り替える。
static void apply_detail_mode_visibility(obs_properties_t* props, DelayStreamData* d, bool detail) {
    if (!props || !d) return;
    if (auto* p = obs_properties_get(props, "host_ip_manual")) {
        obs_property_set_visible(p, detail);
    }
    int sub_count = d->sub_ch_count;
    for (int i = 0; i < sub_count; ++i) {
        char uk[32], dk[32], mk[32], ak[32];
        snprintf(uk, sizeof(uk), "sub%d_url", i);
        snprintf(dk, sizeof(dk), "sub%d_delay_ms", i);
        snprintf(mk, sizeof(mk), "sub%d_meas", i);
        snprintf(ak, sizeof(ak), "sub%d_apply", i);
        if (auto* p = obs_properties_get(props, uk)) {
            obs_property_set_visible(p, detail);
        }
        if (auto* p = obs_properties_get(props, dk)) {
            obs_property_set_visible(p, detail);
        }
        if (auto* p = obs_properties_get(props, mk)) {
            obs_property_set_visible(p, detail);
        }
        if (auto* p = obs_properties_get(props, ak)) {
            obs_property_set_visible(p, detail);
        }
    }
}
// 詳細表示モード切り替え時の可視性更新コールバック。
static bool cb_detail_mode_changed(void* priv, obs_properties_t* props, obs_property_t*, obs_data_t* settings) {
    auto* d = static_cast<DelayStreamData*>(priv);
    if (!props || !settings || !d) return false;
    bool detail = obs_data_get_bool(settings, "detail_mode");
    apply_detail_mode_visibility(props, d, detail);
    return true;
}

// Sync Flowの現在フェーズに応じたUIを構築する。
static void build_flow_panel(obs_properties_t* props, DelayStreamData* d) {
    if (!props || !d) return;
    obs_properties_add_text(props, "flow_desc",
        T_("FlowDesc"),
        OBS_TEXT_INFO);
    FlowPhase phase = d->flow.phase();
    FlowResult res  = d->flow.result();
    int sub_count = d->sub_ch_count;
    switch (phase) {
    case FlowPhase::Idle:
    case FlowPhase::Complete: {
        if (phase == FlowPhase::Complete)
            obs_properties_add_text(props, "flow_complete", T_("FlowComplete"), OBS_TEXT_INFO);
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", T_("FlowConnected"));
        int nc = 0;
        for (int i = 0; i < sub_count; ++i)
            if (d->router.client_count(i) > 0) {
                char t[8]; snprintf(t, sizeof(t), "Ch.%d ", i+1);
                strncat(buf, t, sizeof(buf)-strlen(buf)-1); ++nc;
            }
        if (nc == 0) strncat(buf, T_("FlowNone"), sizeof(buf)-strlen(buf)-1);
        obs_properties_add_text(props, "flow_connected", buf, OBS_TEXT_INFO);
        if (nc > 0)
            obs_properties_add_button2(props, "flow_start_btn",
                T_("FlowStart"), cb_flow_start, d);
        if (phase == FlowPhase::Complete)
            obs_properties_add_button2(props, "flow_reset_btn", T_("Reset"), cb_flow_reset, d);
        break;
    }
    case FlowPhase::Step1_Measuring: {
        char buf[512];
        snprintf(buf, sizeof(buf), T_("FlowStep1ProgressFmt"),
                 res.measured_count, res.connected_count);
        obs_properties_add_text(props, "flow_s1_prog", buf, OBS_TEXT_INFO);
        obs_properties_add_button2(props, "flow_cancel_s1m", T_("Cancel"), cb_flow_reset, d);
        break;
    }
    case FlowPhase::Step1_Done: {
        char buf[1024];
        snprintf(buf, sizeof(buf), T_("FlowStep1DoneFmt"), res.max_one_way_ms);
        for (int i = 0; i < sub_count; ++i) {
            if (!res.channels[i].connected) continue;
            char line[80];
            if (res.channels[i].measured)
                snprintf(line, sizeof(line), "\n  Ch%-2d %.1f ms -> +%.1f ms",
                         i+1, res.channels[i].one_way_ms, res.channels[i].proposed_delay);
            else
                snprintf(line, sizeof(line), "\n  Ch%-2d %s", i+1, T_("FlowChFailed"));
            strncat(buf, line, sizeof(buf)-strlen(buf)-1);
        }
        obs_properties_add_text(props, "flow_s1_result", buf, OBS_TEXT_INFO);
        // 失敗CHがあればリトライボタンを表示
        if (res.measured_count < res.connected_count)
            obs_properties_add_button2(props, "flow_retry_btn",
                T_("FlowRetryFailed"), cb_flow_retry_failed, d);
        obs_properties_add_button2(props, "flow_apply2_btn",
            T_("FlowApplyStep2"), cb_flow_apply_step2, d);
        obs_properties_add_button2(props, "flow_cancel_s1d", T_("Cancel"), cb_flow_reset, d);
        break;
    }
    case FlowPhase::Step2_Applied: {
        char buf[256];
        snprintf(buf, sizeof(buf), T_("FlowStep2DoneFmt"), res.max_one_way_ms);
        obs_properties_add_text(props, "flow_s2_done", buf, OBS_TEXT_INFO);
        obs_properties_add_button2(props, "flow_s3_btn",
            T_("FlowStep3Start"), cb_flow_start_step3, d);
        obs_properties_add_button2(props, "flow_cancel_s2a", T_("Cancel"), cb_flow_reset, d);
        break;
    }
    case FlowPhase::Step3_Measuring:
        obs_properties_add_text(props, "flow_s3_prog", T_("FlowStep3Measuring"), OBS_TEXT_INFO);
        obs_properties_add_button2(props, "flow_cancel_s3m", T_("Cancel"), cb_flow_reset, d);
        break;
    case FlowPhase::Step3_Done: {
        char buf[512];
        if (res.rtmp_valid) {
            snprintf(buf, sizeof(buf),
                T_("FlowStep3ResultFmt"),
                res.rtmp_one_way_ms, res.max_one_way_ms, res.master_delay_ms);
            obs_properties_add_text(props, "flow_s3_result", buf, OBS_TEXT_INFO);
            char al[80];
            snprintf(al, sizeof(al), T_("FlowApplyMasterFmt"), res.master_delay_ms);
            obs_properties_add_button2(props, "flow_apply3_btn", al, cb_flow_apply_step3, d);
        } else {
            snprintf(buf, sizeof(buf), T_("FlowRtmpFailedFmt"), res.rtmp_error.c_str());
            obs_properties_add_text(props, "flow_s3_err", buf, OBS_TEXT_INFO);
            obs_properties_add_button2(props, "flow_retry3_btn", T_("FlowRetry"), cb_flow_start_step3, d);
        }
        obs_properties_add_button2(props, "flow_cancel_s3d", T_("Cancel"), cb_flow_reset, d);
        break;
    }
    }
}

// プロパティ表示に必要な最小状態（配信ID有無/詳細モード）を取得する。
static void read_properties_view_state(DelayStreamData* d, bool& has_sid, bool& detail_mode) {
    has_sid = false;
    detail_mode = false;
    if (!d || !d->context) return;
    obs_data_t* s = obs_source_get_settings(d->context);
    if (!s) return;
    const char* sid = obs_data_get_string(s, "stream_id");
    has_sid = (sid && *sid);
    detail_mode = obs_data_get_bool(s, "detail_mode");
    obs_data_release(s);
}

// 詳細モード切替のトグルを追加する。
static void add_detail_mode_property(obs_properties_t* props, DelayStreamData* d) {
    obs_property_t* detail_p =
        obs_properties_add_bool(props, "detail_mode", T_("DetailMode"));
    obs_property_set_modified_callback2(detail_p, cb_detail_mode_changed, d);
}

// Stream ID / IP 設定グループ
static void add_stream_group(obs_properties_t* props, DelayStreamData* d) {
    if (!props || !d) return;
    obs_properties_t* grp = obs_properties_create();
    obs_property_t* sid_p =
        obs_properties_add_text(grp, "stream_id", T_("StreamId"), OBS_TEXT_DEFAULT);
    obs_property_set_modified_callback2(sid_p, cb_stream_id_changed, d);
    // 配信IDは自動生成のみ。UIからの編集は不可にする
    obs_property_set_enabled(sid_p, false);
    {
        char info[128];
        snprintf(info, sizeof(info), T_("AutoIpFmt"), d->auto_ip.c_str());
        obs_properties_add_text(grp, "auto_ip_info", info, OBS_TEXT_INFO);
    }
    obs_property_t* ip_p =
        obs_properties_add_text(grp, "host_ip_manual", T_("IpOverride"), OBS_TEXT_DEFAULT);
    if (d->router_running.load()) {
        obs_property_set_enabled(sid_p, false);
        obs_property_set_enabled(ip_p, false);
    }
    obs_properties_add_group(props, "grp_stream", T_("GroupStreamId"), OBS_GROUP_NORMAL, grp);
}

// WebSocket 設定グループ
static void add_ws_group(obs_properties_t* props, DelayStreamData* d, bool has_sid) {
    if (!props || !d) return;
    bool ws_running = d->router_running.load();

    char ws_title[96];
    if (ws_running)
        snprintf(ws_title, sizeof(ws_title), T_("WsRunningFmt"), WS_PORT);
    else
        snprintf(ws_title, sizeof(ws_title), "%s", T_("WsStopped"));
    if (ws_running && !d->ws_send_enabled.load()) {
        strncat(ws_title, T_("WsPausedSuffix"),
            sizeof(ws_title) - strlen(ws_title) - 1);
    }

    obs_properties_t* grp = obs_properties_create();
    obs_property_t* codec_p = obs_properties_add_list(
        grp, "audio_codec", T_("AudioCodec"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(codec_p, "Opus (WebCodecs)", 0);
    obs_property_list_add_int(codec_p, T_("CodecPcm"), 1);
    if (ws_running) {
        obs_property_set_enabled(codec_p, false);
    }

    if (ws_running) {
        obs_properties_add_button2(grp, "ws_server_stop_btn",
            T_("WsServerStop"), cb_ws_server_stop, d);
    } else {
        obs_property_t* start_p = obs_properties_add_button2(grp, "ws_server_start_btn",
            T_("WsServerStart"), cb_ws_server_start, d);
        obs_property_set_enabled(start_p, has_sid);
        obs_property_t* note_p =
            obs_properties_add_text(grp, "ws_server_start_note_sid",
                T_("WsServerStartNoteSid"), OBS_TEXT_INFO);
        obs_property_set_visible(note_p, !has_sid);
    }

    obs_property_t* send_p = obs_properties_add_bool(grp, "ws_send_paused", T_("WsSendPause"));
    if (!ws_running) {
        obs_property_set_enabled(send_p, false);
    }
    obs_property_t* delay_p = obs_properties_add_bool(grp, "delay_disable", T_("DelayDisable"));
    if (!ws_running) {
        obs_property_set_enabled(delay_p, false);
    }
    obs_properties_add_group(props, "grp_ws", ws_title, OBS_GROUP_NORMAL, grp);
}

// Tunnel 設定グループ
static void add_tunnel_group(obs_properties_t* props, DelayStreamData* d) {
    if (!props || !d) return;
    TunnelState ts = d->tunnel.state();
    std::string turl = d->tunnel.url();
    std::string terr = d->tunnel.error();
    const char* tunnel_title =
        (ts == TunnelState::Running)
            ? T_("TunnelRunning")
            : T_("TunnelStopped");

    obs_properties_t* grp = obs_properties_create();
    obs_property_t* type_p = obs_properties_add_list(grp, "tunnel_type", T_("TunnelService"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(type_p, "ngrok (TCP)", 0);
    obs_property_list_add_int(type_p, "cloudflared (free)", 1);
    obs_properties_add_text(grp, "ngrok_exe_path", T_("NgrokExePath"), OBS_TEXT_DEFAULT);
    obs_properties_add_text(grp, "ngrok_token", T_("NgrokToken"), OBS_TEXT_PASSWORD);
    obs_properties_add_text(grp, "cloudflared_exe_path", T_("CloudflaredExePath"), OBS_TEXT_DEFAULT);
    obs_property_set_modified_callback(type_p, cb_tunnel_type_changed);

    int tunnel_type = 0;
    {
        obs_data_t* s = obs_source_get_settings(d->context);
        tunnel_type = (int)obs_data_get_int(s, "tunnel_type");
        cb_tunnel_type_changed(grp, nullptr, s);
        obs_data_release(s);
    }

    bool cloudflared_downloading =
        (tunnel_type == 1) && d->tunnel.cloudflared_downloading();
    if (ts == TunnelState::Running) {
        obs_properties_add_button2(grp, "tunnel_stop_btn",
            T_("TunnelStop"), cb_tunnel_stop, d);
    } else if (cloudflared_downloading) {
        obs_property_t* dl_p =
            obs_properties_add_button2(grp, "tunnel_downloading_btn",
                T_("CloudflaredDownloading"), cb_tunnel_stop, d);
        obs_property_set_enabled(dl_p, false);
    } else if (ts == TunnelState::Starting) {
        obs_property_t* starting_p =
            obs_properties_add_button2(grp, "tunnel_starting_btn",
                T_("TunnelStarting"), cb_tunnel_stop, d);
        obs_property_set_enabled(starting_p, false);
    } else {
        obs_property_t* start_p =
            obs_properties_add_button2(grp, "tunnel_start_btn", T_("TunnelStart"), cb_tunnel_start, d);
        bool ws_running = d->router_running.load();
        obs_property_set_enabled(start_p, ws_running);
        if (!ws_running) {
            obs_properties_add_text(grp, "tunnel_start_note",
                T_("TunnelStartNote"), OBS_TEXT_INFO);
        }
    }
    if (ts == TunnelState::Running && !turl.empty()) {
        // URL 表示は「演者別チャンネル」に集約
    } else if (ts == TunnelState::Error && !terr.empty()) {
        char eb[256];
        snprintf(eb, sizeof(eb), T_("TunnelErrorFmt"), terr.c_str());
        obs_properties_add_text(grp, "tunnel_error", eb, OBS_TEXT_INFO);
    }
    obs_properties_add_group(props, "grp_tunnel", tunnel_title, OBS_GROUP_NORMAL, grp);
}

// Sync Flowグループを追加する。
static void add_flow_group(obs_properties_t* props, DelayStreamData* d) {
    if (!props || !d) return;
    obs_properties_t* grp = obs_properties_create();
    build_flow_panel(grp, d);
    obs_properties_add_group(props, "grp_flow", T_("GroupSyncFlow"), OBS_GROUP_NORMAL, grp);
}

// Master遅延/RTMP測定グループを追加する。
static void add_master_group(obs_properties_t* props, DelayStreamData* d) {
    if (!props || !d) return;
    obs_properties_t* grp = obs_properties_create();
    obs_property_t* mp = obs_properties_add_float_slider(
        grp, "master_delay_ms", T_("MasterDelay"), 0.0, MAX_DELAY_MS, 1.0);
    obs_property_float_set_suffix(mp, " ms");
    obs_properties_add_text(grp, "rtmp_url_manual", T_("RtmpUrlManual"), OBS_TEXT_DEFAULT);
    obs_property_t* rtmp_p = obs_properties_add_button2(
        grp, "rtmp_measure_btn",
        d->rtmp.prober.is_running() ? T_("Measuring") : T_("RtmpMeasure"),
        cb_rtmp_measure, d);
    if (!d->router_running.load()) {
        obs_property_set_enabled(rtmp_p, false);
    }
    {
        std::lock_guard<std::mutex> lk(d->rtmp.mtx);
        if (d->rtmp.result.valid) {
            char res[128];
            snprintf(res, sizeof(res), T_("RtmpResultFmt"),
                     d->rtmp.result.avg_rtt_ms, d->rtmp.result.avg_one_way);
            obs_properties_add_text(grp, "rtmp_result", res, OBS_TEXT_INFO);
            char al[64];
            snprintf(al, sizeof(al), T_("ApplyFmt"), d->rtmp.result.avg_one_way);
            obs_properties_add_button2(grp, "rtmp_apply_btn", al, cb_rtmp_apply, d);
        } else {
            obs_properties_add_text(grp, "rtmp_no_result", T_("RtmpNoResult"), OBS_TEXT_INFO);
        }
    }
    obs_properties_add_group(props, "grp_master", T_("GroupMasterRtmp"), OBS_GROUP_NORMAL, grp);
}

// Sub-CH共通オフセット設定グループを追加する。
static void add_sub_offset_group(obs_properties_t* props, DelayStreamData* d) {
    if (!props || !d) return;
    obs_properties_t* grp = obs_properties_create();
    {
        char offset_info[256];
        snprintf(offset_info, sizeof(offset_info),
            T_("SubOffsetInfoFmt"), d->sub_offset_ms);
        obs_properties_add_text(grp, "sub_offset_info", offset_info, OBS_TEXT_INFO);
    }
    obs_property_t* op = obs_properties_add_float_slider(
        grp, "sub_offset_ms",
        T_("SubOffsetLabel"), -2000.0, 5000.0, 10.0);
    obs_property_float_set_suffix(op, " ms");
    obs_properties_add_group(props, "grp_offset",
        T_("GroupSubOffset"), OBS_GROUP_NORMAL, grp);
}

// Sub-CHの測定状態に応じてボタン/結果表示を切り替える。
static void add_sub_channel_measure_controls(obs_properties_t* ch_grp,
                                             DelayStreamData* d,
                                             int i,
                                             size_t nc,
                                             const char* mk,
                                             const char* ak,
                                             const char* rk) {
    if (nc > 0) {
        MeasureState& ms = d->sub[i].measure;
        std::lock_guard<std::mutex> lk(ms.mtx);
        if (ms.measuring) {
            obs_property_t* mp = obs_properties_add_button2(
                ch_grp, mk, T_("Measuring"),
                cb_sub_measure, &d->btn_ctx[i]);
            obs_property_set_enabled(mp, false);
        } else if (ms.result.valid) {
            char rs[128];
            snprintf(rs, sizeof(rs), T_("SubResultFmt"),
                     ms.result.avg_rtt_ms, ms.result.avg_one_way);
            obs_properties_add_text(ch_grp, rk, rs, OBS_TEXT_INFO);
            char al[64];
            snprintf(al, sizeof(al), T_("ApplyFmt"), ms.result.avg_one_way);
            obs_properties_add_button2(ch_grp, ak, al, cb_sub_apply, &d->btn_ctx[i]);
            char ml[64];
            snprintf(ml, sizeof(ml), T_("SubRemeasureFmt"), nc);
            obs_properties_add_button2(ch_grp, mk, ml,
                cb_sub_measure, &d->btn_ctx[i]);
        } else {
            if (!ms.last_error.empty()) {
                obs_properties_add_text(ch_grp, rk, ms.last_error.c_str(), OBS_TEXT_INFO);
            }
            char ml[64];
            snprintf(ml, sizeof(ml), T_("SubMeasureFmt"), nc);
            obs_properties_add_button2(ch_grp, mk, ml,
                cb_sub_measure, &d->btn_ctx[i]);
        }
        return;
    }

    obs_property_t* mp = obs_properties_add_button2(
        ch_grp, mk, T_("SubMeasureDisconnected"),
        cb_sub_measure, &d->btn_ctx[i]);
    obs_property_set_enabled(mp, false);
}

// Sub-CH 1件分のUIを構築する。
static void add_sub_channel_item(obs_properties_t* grp, DelayStreamData* d, int i, int sub_count) {
    if (!grp || !d) return;
    d->btn_ctx[i] = { d, i };

    char dk[32], uk[32], mk[32], ak[32], rk[32], nk[32], dk_rm[32];
    char dn[64], ul[32], us[512], gk[32], gt[32];
    snprintf(dk, sizeof(dk), "sub%d_delay_ms", i);
    snprintf(dn, sizeof(dn), "%s", T_("SubDelay"));
    snprintf(uk, sizeof(uk), "sub%d_url", i);
    snprintf(mk, sizeof(mk), "sub%d_meas", i);
    snprintf(ak, sizeof(ak), "sub%d_apply", i);
    snprintf(rk, sizeof(rk), "sub%d_result", i);
    snprintf(nk, sizeof(nk), "sub%d_memo", i);
    snprintf(dk_rm, sizeof(dk_rm), "sub%d_remove", i);
    snprintf(ul, sizeof(ul), "URL");
    snprintf(gk, sizeof(gk), "sub%d_group", i);
    snprintf(gt, sizeof(gt), "Ch.%d", i + 1);

    obs_properties_t* ch_grp = obs_properties_create();
    std::string url = make_sub_url(d, i + 1);
    size_t nc = d->router.client_count(i);
    if (url.empty()) {
        snprintf(us, sizeof(us), "%s", T_("NotConfigured"));
    } else {
        snprintf(us, sizeof(us), "<a href=\"%s\">%s</a>", url.c_str(), url.c_str());
    }

    obs_property_t* memo_p = obs_properties_add_text(ch_grp, nk, T_("SubMemo"), OBS_TEXT_DEFAULT);
    if (d->router_running.load()) {
        obs_property_set_enabled(memo_p, false);
    }
    obs_property_t* up = obs_properties_add_text(ch_grp, uk, ul, OBS_TEXT_INFO);
    obs_property_set_long_description(up, us);
    obs_property_text_set_info_word_wrap(up, false);
    obs_property_t* sp = obs_properties_add_float_slider(ch_grp, dk, dn, 0.0, MAX_DELAY_MS, 1.0);
    obs_property_float_set_suffix(sp, " ms");

    add_sub_channel_measure_controls(ch_grp, d, i, nc, mk, ak, rk);

    char rm_label[32];
    snprintf(rm_label, sizeof(rm_label), T_("SubRemoveFmt"), i + 1);
    obs_property_t* rm = obs_properties_add_button2(ch_grp, dk_rm, rm_label,
        cb_sub_remove, &d->btn_ctx[i]);
    obs_property_set_long_description(rm, T_("SubRemoveDesc"));
    if (d->router_running.load() || sub_count <= 1) {
        obs_property_set_enabled(rm, false);
    }
    obs_properties_add_group(grp, gk, gt, OBS_GROUP_NORMAL, ch_grp);
}

// Sub-CH 一覧グループ
static void add_sub_channels_group(obs_properties_t* props, DelayStreamData* d) {
    if (!props || !d) return;
    obs_properties_t* grp = obs_properties_create();
    {
        char copy_all_label[128];
        snprintf(copy_all_label, sizeof(copy_all_label), T_("SubCopyAll"), d->sub_ch_count);
        obs_properties_add_button2(grp, "sub_copy_all", copy_all_label, cb_sub_copy_all, d);
    }
    {
        obs_property_t* spc = obs_properties_add_text(grp, "sub_copy_all_spacer", "", OBS_TEXT_INFO);
        obs_property_set_long_description(spc, " ");
        obs_property_text_set_info_word_wrap(spc, false);
    }
    int sub_count = d->sub_ch_count;
    for (int i = 0; i < sub_count; ++i) {
        add_sub_channel_item(grp, d, i, sub_count);
    }
    {
        obs_property_t* spc = obs_properties_add_text(grp, "sub_add_spacer", "", OBS_TEXT_INFO);
        obs_property_set_long_description(spc, " ");
        obs_property_text_set_info_word_wrap(spc, false);
    }
    char add_label[32];
    snprintf(add_label, sizeof(add_label), T_("SubAddFmt"), d->sub_ch_count + 1);
    obs_property_t* add_p =
        obs_properties_add_button2(grp, "sub_add_btn", add_label, cb_sub_add, d);
    if (d->router_running.load() || d->sub_ch_count >= NUM_SUB_CH) {
        obs_property_set_enabled(add_p, false);
    }
    char sub_group_label[128];
    snprintf(sub_group_label, sizeof(sub_group_label),
             T_("GroupSubChannels"), d->sub_ch_count);
    obs_properties_add_group(props, "grp_sub", sub_group_label, OBS_GROUP_NORMAL, grp);
}

// OBSプロパティパネル全体を構築する。
static obs_properties_t* ds_get_properties(void* data) {
    obs_properties_t* props = obs_properties_create();
    if (!data) return props;
    auto* d = static_cast<DelayStreamData*>(data);
    // 再入防止（OBSのUI更新タイミングで重複呼び出しされることがある）
    if (d->in_get_props.exchange(true)) {
        return props;
    }

    maybe_fill_cloudflared_path_from_auto(d);

    bool has_sid = false;
    bool detail_mode = false;
    read_properties_view_state(d, has_sid, detail_mode);

    // UIブロックを順に組み立てる
    add_detail_mode_property(props, d);
    add_stream_group(props, d);
    add_ws_group(props, d, has_sid);
    add_tunnel_group(props, d);
    add_flow_group(props, d);
    add_master_group(props, d);
    add_sub_offset_group(props, d);
    add_sub_channels_group(props, d);

    apply_detail_mode_visibility(props, d, detail_mode);

    obs_properties_add_text(props, "about_info",
        "obs-delay-stream v" PLUGIN_VERSION " | (C) 2026 Mazzn1987, Chigiri Tsutsumi | GPL 2.0+",
        OBS_TEXT_INFO);

    d->in_get_props.store(false);
    return props;
}

// 各設定項目のデフォルト値を定義する。
static void ds_get_defaults(obs_data_t* settings) {
    obs_data_set_default_bool  (settings, "enabled",               true);
    obs_data_set_default_bool  (settings, "delay_disable",         false);
    obs_data_set_default_bool  (settings, "detail_mode",           false);
    obs_data_set_default_bool  (settings, "ws_send_paused",        false);
    obs_data_set_default_bool  (settings, "ws_send_enabled",       true);
    obs_data_set_default_bool  (settings, "ws_enabled",            true);
    obs_data_set_default_int   (settings, "sub_ch_count",          1);
    obs_data_set_default_int   (settings, "audio_codec",           0);
    obs_data_set_default_string(settings, "stream_id",             "");
    obs_data_set_default_string(settings, "host_ip_manual",        "");
    obs_data_set_default_double(settings, "master_delay_ms",       0.0);
    obs_data_set_default_double(settings, "sub_offset_ms",         0.0);
    obs_data_set_default_string(settings, "rtmp_url_manual",       "");
    obs_data_set_default_int   (settings, "tunnel_type",           1);
    obs_data_set_default_string(settings, "ngrok_exe_path",        "C:\\ngrok\\ngrok.exe");
    obs_data_set_default_string(settings, "ngrok_token",           "");
    obs_data_set_default_string(settings, "cloudflared_exe_path",  "auto");
    for (int i = 0; i < NUM_SUB_CH; ++i) {
        char key[32]; snprintf(key, sizeof(key), "sub%d_delay_ms", i);
        obs_data_set_default_double(settings, key, 0.0);
        char memo_key[32]; snprintf(memo_key, sizeof(memo_key), "sub%d_memo", i);
        obs_data_set_default_string(settings, memo_key, "");
    }
}
