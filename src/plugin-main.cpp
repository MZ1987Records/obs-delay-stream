/*
 * obs-delay-stream  v2.0.0
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

static void copy_to_clipboard(const std::string& text) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (h) {
        char* p = static_cast<char*>(GlobalLock(h));
        if (p) { memcpy(p, text.c_str(), text.size() + 1); GlobalUnlock(h); SetClipboardData(CF_TEXT, h); }
    }
    CloseClipboard();
}

static std::string sanitize_stream_id(const char* raw) {
    if (!raw) return "";
    std::string s(raw);
    s.erase(std::remove_if(s.begin(), s.end(), [](char c){
        return !std::isalnum((unsigned char)c) && c != '-' && c != '_';
    }), s.end());
    return s;
}

static std::string read_file_to_string(const char* path) {
    if (!path || !*path) return "";
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return "";
    return std::string((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());
}

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
    std::atomic<bool> ws_enabled{true};
    mutable std::mutex stream_id_mtx;  // protects stream_id, host_ip
    std::string   stream_id;
    std::string   host_ip;
    std::string   auto_ip;
    float         master_delay_ms = 0.0f;
    float         sub_offset_ms   = 0.0f; // global offset added to all sub-ch after Sync Flow
    DelayBuffer   master_buf;
    RtmpMeasureState rtmp;
    StreamRouter  router;
    std::atomic<bool> router_running{false};
    std::array<ChCtx, NUM_SUB_CH> btn_ctx;
    std::array<ChCtx, NUM_SUB_CH> copy_ctx;
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
    std::vector<float> work_buf;

    // Thread-safe accessors for stream_id
    std::string get_stream_id() const {
        std::lock_guard<std::mutex> lk(stream_id_mtx);
        return stream_id;
    }
    void set_stream_id(const std::string& id) {
        std::lock_guard<std::mutex> lk(stream_id_mtx);
        stream_id = id;
    }
    std::string get_host_ip() const {
        std::lock_guard<std::mutex> lk(stream_id_mtx);
        return host_ip;
    }
};

static void request_properties_refresh(DelayStreamData* d) {
    if (!d || !d->context || !d->create_done.load()) return;
    if (d->in_get_props.load()) return; // prevent re-entry
    obs_source_t* ctx = d->context;
    obs_queue_task(OBS_TASK_UI, [](void* p) {
        obs_source_update_properties(static_cast<obs_source_t*>(p));
    }, ctx, false);
}

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

static void write_master_delay(DelayStreamData* d, double ms) {
    obs_data_t* s = obs_source_get_settings(d->context);
    obs_data_set_double(s, "master_delay_ms", ms);
    d->master_delay_ms = (float)ms;
    if (d->enabled.load()) d->master_buf.set_delay_ms((uint32_t)ms);
    obs_data_release(s);
}

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

static bool cb_sub_measure(obs_properties_t*, obs_property_t*, void*);
static bool cb_sub_apply(obs_properties_t*, obs_property_t*, void*);
static bool cb_sub_copy_url(obs_properties_t*, obs_property_t*, void*);
static bool cb_rtmp_measure(obs_properties_t*, obs_property_t*, void*);
static bool cb_rtmp_apply(obs_properties_t*, obs_property_t*, void*);
static bool cb_tunnel_start(obs_properties_t*, obs_property_t*, void*);
static bool cb_tunnel_stop(obs_properties_t*, obs_property_t*, void*);
static bool cb_flow_start(obs_properties_t*, obs_property_t*, void*);
static bool cb_flow_apply_step2(obs_properties_t*, obs_property_t*, void*);
static bool cb_flow_start_step3(obs_properties_t*, obs_property_t*, void*);
static bool cb_flow_apply_step3(obs_properties_t*, obs_property_t*, void*);
static bool cb_flow_reset(obs_properties_t*, obs_property_t*, void*);
static bool cb_enabled_changed(void*, obs_properties_t*, obs_property_t*, obs_data_t*);
static bool cb_ws_enabled_changed(void*, obs_properties_t*, obs_property_t*, obs_data_t*);
static bool cb_stream_id_changed(void*, obs_properties_t*, obs_property_t*, obs_data_t*);
static bool cb_host_ip_changed(void*, obs_properties_t*, obs_property_t*, obs_data_t*);
static bool cb_ws_server_start(obs_properties_t*, obs_property_t*, void*);
static bool cb_ws_server_stop(obs_properties_t*, obs_property_t*, void*);

static struct obs_source_info delay_stream_filter;

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

bool obs_module_load(void) {
    register_source_info();
    obs_register_source(&delay_stream_filter);
    blog(LOG_INFO, "[obs-delay-stream] v2.0.0 loaded");
    return true;
}
void obs_module_unload(void) {}

static const char* ds_get_name(void*) { return "Delay Stream (obs-delay-stream)"; }

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
        d->copy_ctx[i]        = { d, i };
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
    d->router.on_conn_change = [d](const std::string& sid, int, size_t) {
        if (sid == d->get_stream_id()) request_properties_refresh(d);
    };
    d->router.on_any_latency_result = [d](const std::string& sid, int ch, LatencyResult r) {
        if (sid != d->get_stream_id()) return;
        if (ch < 0 || ch >= NUM_SUB_CH) return;
        auto& ms = d->sub[ch].measure;
        {
            std::lock_guard<std::mutex> lk(ms.mtx);
            ms.result = r;
            ms.measuring = false;
            ms.applied = false;
            ms.last_error = r.valid ? "" : T_("MeasureFailed");
        }
        request_properties_refresh(d);
    };

    ds_update(d, settings);
    d->create_done.store(true);
    blog(LOG_INFO, "[obs-delay-stream] ds_create complete");
    return d;
}

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
    d->router.clear_callbacks();

    delete d;
}

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

static void ds_update(void* data, obs_data_t* settings) {
    auto* d = static_cast<DelayStreamData*>(data);
    d->enabled.store(obs_data_get_bool(settings, "enabled"));
    d->ws_enabled.store(obs_data_get_bool(settings, "ws_enabled"));
    int audio_codec = (int)obs_data_get_int(settings, "audio_codec");
    d->router.set_audio_codec(audio_codec);
    const char* raw = obs_data_get_string(settings, "stream_id");
    std::string sid = raw ? sanitize_stream_id(raw) : "";
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
        // Apply base delay + global offset (clamp to 0)
        float effective = d->sub[i].delay_ms + d->sub_offset_ms;
        if (effective < 0.0f) effective = 0.0f;
        d->sub[i].buf.set_delay_ms(d->enabled.load() ? (uint32_t)effective : 0);
    }
}

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
    bool ws = d->ws_enabled.load(std::memory_order_relaxed);
    bool rr = d->router_running.load(std::memory_order_relaxed);
    bool has_sid = !d->get_stream_id().empty();

    if (en) {
        d->master_buf.process(in, out, frames);
        for (uint32_t c = 0; c < ch; ++c) {
            if (!audio->data[c]) continue;
            float* dst = reinterpret_cast<float*>(audio->data[c]);
            for (size_t f = 0; f < frames; ++f) dst[f] = out[f*ch+c];
        }
        if (ws && rr && has_sid) {
            for (int i = 0; i < NUM_SUB_CH; ++i) {
                d->sub[i].buf.process(in, sub, frames);
                d->router.send_audio(i, sub, frames, sr, ch);
            }
        }
    } else {
        if (ws && rr && has_sid)
            for (int i = 0; i < NUM_SUB_CH; ++i)
                d->router.send_audio(i, in, frames, sr, ch);
    }
    return audio;
}

// Button callbacks
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
static bool cb_sub_apply(obs_properties_t*, obs_property_t*, void* priv) {
    auto* ctx = static_cast<ChCtx*>(priv);
    auto* d = ctx->d; int i = ctx->ch;
    LatencyResult r;
    { std::lock_guard<std::mutex> lk(d->sub[i].measure.mtx); r = d->sub[i].measure.result; }
    if (!r.valid) return false;
    obs_data_t* s = obs_source_get_settings(d->context);
    char key[32]; snprintf(key, sizeof(key), "sub%d_delay_ms", i);
    obs_data_set_double(s, key, r.avg_one_way);
    d->sub[i].delay_ms = (float)r.avg_one_way;
    d->sub[i].buf.set_delay_ms((uint32_t)r.avg_one_way);
    obs_data_release(s);
    d->router.notify_apply_delay(i, r.avg_one_way);
    request_properties_refresh(d);
    return false;
}
static bool cb_sub_copy_url(obs_properties_t*, obs_property_t*, void* priv) {
    auto* ctx = static_cast<ChCtx*>(priv);
    std::string url = make_sub_url(ctx->d, ctx->ch + 1);
    if (url.empty()) return false;
    copy_to_clipboard(url);
    return false;
}
static bool cb_rtmp_measure(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    if (d->rtmp.prober.is_running()) return false;
    std::string url = get_obs_stream_url();
    if (url.empty()) {
        obs_data_t* s = obs_source_get_settings(d->context);
        const char* m = obs_data_get_string(s, "rtmp_url_manual");
        if (m && *m) url = m;
        obs_data_release(s);
    }
    if (url.empty()) return false;
    { std::lock_guard<std::mutex> lk(d->rtmp.mtx); d->rtmp.cached_url = url; }
    d->rtmp.prober.start(url, RTMP_PROBE_CNT, RTMP_PROBE_INTV);
    return false;
}
static bool cb_rtmp_apply(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    RtmpProbeResult r;
    { std::lock_guard<std::mutex> lk(d->rtmp.mtx); r = d->rtmp.result; }
    if (!r.valid) return false;
    write_master_delay(d, r.avg_one_way);
    request_properties_refresh(d);
    return false;
}
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

static bool cb_ws_server_stop(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    if (!d->router_running.load()) return false;
    d->router.stop();
    d->router_running.store(false);
    blog(LOG_INFO, "[obs-delay-stream] WebSocket server stopped");
    request_properties_refresh(d);
    return false;
}

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
static bool cb_tunnel_stop(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    d->tunnel.stop();
    request_properties_refresh(d);
    return false;
}
static bool cb_flow_start(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    std::string sid = d->get_stream_id();
    if (sid.empty()) return false;
    d->flow.start_step1(d->router, sid);
    return false;
}
static bool cb_flow_apply_step2(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    d->flow.apply_step2();
    request_properties_refresh(d);
    return false;
}
static bool cb_flow_start_step3(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    std::string url = get_obs_stream_url();
    if (url.empty()) {
        obs_data_t* s = obs_source_get_settings(d->context);
        const char* m = obs_data_get_string(s, "rtmp_url_manual");
        if (m && *m) url = m;
        obs_data_release(s);
    }
    d->flow.start_step3(url);
    return false;
}
static bool cb_flow_apply_step3(obs_properties_t*, obs_property_t*, void* priv) {
    static_cast<DelayStreamData*>(priv)->flow.apply_step3();
    return false;
}
static bool cb_flow_reset(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    d->flow.reset();
    request_properties_refresh(d);
    return false;
}
static bool cb_enabled_changed(void* priv, obs_properties_t*, obs_property_t*, obs_data_t* settings) {
    auto* d = static_cast<DelayStreamData*>(priv);
    if (!d) return false;
    bool en = obs_data_get_bool(settings, "enabled");
    d->enabled.store(en);
    d->master_buf.set_delay_ms(en ? (uint32_t)d->master_delay_ms : 0);
    for (int i = 0; i < NUM_SUB_CH; ++i)
        d->sub[i].buf.set_delay_ms(en ? (uint32_t)d->sub[i].delay_ms : 0);
    request_properties_refresh(d);
    return false;
}
static bool cb_ws_enabled_changed(void* priv, obs_properties_t*, obs_property_t*, obs_data_t* settings) {
    auto* d = static_cast<DelayStreamData*>(priv);
    if (!d) return false;
    d->ws_enabled.store(obs_data_get_bool(settings, "ws_enabled"));
    request_properties_refresh(d);
    return false;
}
static bool cb_stream_id_changed(void* priv, obs_properties_t*, obs_property_t*, obs_data_t*) {
    (void)priv; return false;
}
static bool cb_host_ip_changed(void* priv, obs_properties_t*, obs_property_t*, obs_data_t*) {
    (void)priv; return false;
}

static void build_flow_panel(obs_properties_t* props, DelayStreamData* d) {
    if (!props || !d) return;
    obs_properties_add_text(props, "flow_desc",
        T_("FlowDesc"),
        OBS_TEXT_INFO);
    FlowPhase phase = d->flow.phase();
    FlowResult res  = d->flow.result();
    switch (phase) {
    case FlowPhase::Idle:
    case FlowPhase::Complete: {
        if (phase == FlowPhase::Complete)
            obs_properties_add_text(props, "flow_complete", T_("FlowComplete"), OBS_TEXT_INFO);
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", T_("FlowConnected"));
        int nc = 0;
        for (int i = 0; i < NUM_SUB_CH; ++i)
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
        for (int i = 0; i < NUM_SUB_CH; ++i) {
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

static obs_properties_t* ds_get_properties(void* data) {
    obs_properties_t* props = obs_properties_create();
    if (!data) return props;
    auto* d = static_cast<DelayStreamData*>(data);
    // Prevent re-entrant calls
    if (d->in_get_props.exchange(true)) {
        return props; // already building properties
    }

    // (1) WebSocket
    {
        char ws_title[64];
        if (d->router_running.load())
            snprintf(ws_title, sizeof(ws_title), T_("WsRunningFmt"), WS_PORT);
        else
            snprintf(ws_title, sizeof(ws_title), "%s", T_("WsStopped"));
        obs_properties_t* grp = obs_properties_create();
        obs_properties_add_bool(grp, "enabled",
            d->enabled.load() ? T_("DelayOn") : T_("DelayOff"));
        obs_properties_add_bool(grp, "ws_enabled",
            d->ws_enabled.load() ? T_("WsOn") : T_("WsOff"));

        obs_property_t* codec_p = obs_properties_add_list(
            grp, "audio_codec", T_("AudioCodec"),
            OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
        obs_property_list_add_int(codec_p, "Opus (WebCodecs)", 0);
        obs_property_list_add_int(codec_p, T_("CodecPcm"), 1);

        if (d->router_running.load()) {
            obs_properties_add_button2(grp, "ws_server_stop_btn",
                T_("WsServerStop"), cb_ws_server_stop, d);
        } else {
            obs_properties_add_button2(grp, "ws_server_start_btn",
                T_("WsServerStart"), cb_ws_server_start, d);
        }
        obs_properties_add_group(props, "grp_ws", ws_title, OBS_GROUP_NORMAL, grp);
    }

    // (2) Stream ID / IP
    {
        obs_properties_t* grp = obs_properties_create();
        obs_properties_add_text(grp, "stream_id", T_("StreamId"), OBS_TEXT_DEFAULT);
        { char info[128]; snprintf(info, sizeof(info), T_("AutoIpFmt"), d->auto_ip.c_str());
          obs_properties_add_text(grp, "auto_ip_info", info, OBS_TEXT_INFO); }
        obs_properties_add_text(grp, "host_ip_manual", T_("IpOverride"), OBS_TEXT_DEFAULT);
        obs_properties_add_group(props, "grp_stream", T_("GroupStreamId"), OBS_GROUP_NORMAL, grp);
    }

    // (3) Tunnel
    {
        TunnelState ts   = d->tunnel.state();
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
        obs_properties_add_text(grp, "ngrok_exe_path",       T_("NgrokExePath"), OBS_TEXT_DEFAULT);
        obs_properties_add_text(grp, "ngrok_token",          T_("NgrokToken"),  OBS_TEXT_PASSWORD);
        obs_properties_add_text(grp, "cloudflared_exe_path", T_("CloudflaredExePath"), OBS_TEXT_DEFAULT);
        obs_property_set_modified_callback(type_p, cb_tunnel_type_changed);
        {
            obs_data_t* s = obs_source_get_settings(d->context);
            cb_tunnel_type_changed(grp, nullptr, s);
            obs_data_release(s);
        }
        if (ts == TunnelState::Running) {
            obs_properties_add_button2(grp, "tunnel_stop_btn",
                T_("TunnelStop"), cb_tunnel_stop, d);
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
            char eb[256]; snprintf(eb, sizeof(eb), T_("TunnelErrorFmt"), terr.c_str());
            obs_properties_add_text(grp, "tunnel_error", eb, OBS_TEXT_INFO);
        }
        obs_properties_add_group(props, "grp_tunnel", tunnel_title, OBS_GROUP_NORMAL, grp);
    }

    // (4) Sync Flow
    {
        obs_properties_t* grp = obs_properties_create();
        build_flow_panel(grp, d);
        obs_properties_add_group(props, "grp_flow", T_("GroupSyncFlow"), OBS_GROUP_NORMAL, grp);
    }

    // (5) Master / RTMP
    {
        obs_properties_t* grp = obs_properties_create();
        obs_property_t* mp = obs_properties_add_float_slider(
            grp, "master_delay_ms", T_("MasterDelay"), 0.0, MAX_DELAY_MS, 1.0);
        obs_property_float_set_suffix(mp, " ms");
        obs_properties_add_text(grp, "rtmp_url_manual", T_("RtmpUrlManual"), OBS_TEXT_DEFAULT);
        obs_properties_add_button2(grp, "rtmp_measure_btn",
            d->rtmp.prober.is_running() ? T_("Measuring") : T_("RtmpMeasure"),
            cb_rtmp_measure, d);
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

    // (5.5) Sub-CH Global Offset
    {
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

    // (6) Sub channels
    {
        obs_properties_t* grp = obs_properties_create();
        for (int i = 0; i < NUM_SUB_CH; ++i) {
            d->btn_ctx[i]  = { d, i };
            d->copy_ctx[i] = { d, i };
            char dk[32], uk[32], mk[32], ak[32], rk[32], ck[32];
            char dn[64], ul[32], us[512];
            snprintf(dk, sizeof(dk), "sub%d_delay_ms",    i);
            snprintf(dn, sizeof(dn), "%s", T_("SubDelay"));
            snprintf(uk, sizeof(uk), "sub%d_url",         i);
            snprintf(mk, sizeof(mk), "sub%d_meas",        i);
            snprintf(ak, sizeof(ak), "sub%d_apply",       i);
            snprintf(rk, sizeof(rk), "sub%d_result",      i);
            snprintf(ck, sizeof(ck), "sub%d_copy",        i);
            snprintf(ul, sizeof(ul), "Ch.%d", i+1);
            if (i > 0) {
                char sk[32];
                snprintf(sk, sizeof(sk), "sub%d_spacer", i);
                obs_property_t* spc = obs_properties_add_text(grp, sk, "", OBS_TEXT_INFO);
                obs_property_set_long_description(spc, " ");
                obs_property_text_set_info_word_wrap(spc, false);
            }
            std::string url = make_sub_url(d, i+1);
            size_t nc = d->router.client_count(i);
            const char* url_show = url.empty() ? T_("NotConfigured") : url.c_str();
            if (url.empty()) {
                snprintf(us, sizeof(us), "%s", T_("NotConfigured"));
            } else {
                snprintf(us, sizeof(us), "<a href=\"%s\">%s</a>", url.c_str(), url.c_str());
            }
            obs_property_t* up = obs_properties_add_text(grp, uk, ul, OBS_TEXT_INFO);
            obs_property_set_long_description(up, us);
            obs_property_text_set_info_word_wrap(up, false);
            obs_property_t* sp = obs_properties_add_float_slider(grp, dk, dn, 0.0, MAX_DELAY_MS, 1.0);
            obs_property_float_set_suffix(sp, " ms");
            if (!url.empty()) {
                obs_properties_add_button2(grp, ck, T_("SubCopyUrl"), cb_sub_copy_url, &d->copy_ctx[i]);
            }
            if (nc > 0) {
                MeasureState& ms = d->sub[i].measure;
                std::lock_guard<std::mutex> lk(ms.mtx);
                if (ms.measuring) {
                    obs_property_t* mp = obs_properties_add_button2(
                        grp, mk, T_("Measuring"),
                        cb_sub_measure, &d->btn_ctx[i]);
                    obs_property_set_enabled(mp, false);
                } else if (ms.result.valid) {
                    char rs[128];
                    snprintf(rs, sizeof(rs), T_("SubResultFmt"),
                             ms.result.avg_rtt_ms, ms.result.avg_one_way);
                    obs_properties_add_text(grp, rk, rs, OBS_TEXT_INFO);
                    char al[64]; snprintf(al, sizeof(al), T_("ApplyFmt"), ms.result.avg_one_way);
                    obs_properties_add_button2(grp, ak, al, cb_sub_apply, &d->btn_ctx[i]);
                    char ml[64];
                    snprintf(ml, sizeof(ml), T_("SubRemeasureFmt"), nc);
                    obs_properties_add_button2(grp, mk, ml,
                        cb_sub_measure, &d->btn_ctx[i]);
                } else {
                    if (!ms.last_error.empty()) {
                        obs_properties_add_text(grp, rk, ms.last_error.c_str(), OBS_TEXT_INFO);
                    }
                    char ml[64];
                    snprintf(ml, sizeof(ml), T_("SubMeasureFmt"), nc);
                    obs_properties_add_button2(grp, mk, ml,
                        cb_sub_measure, &d->btn_ctx[i]);
                }
            } else {
                obs_property_t* mp = obs_properties_add_button2(
                    grp, mk, T_("SubMeasureDisconnected"),
                    cb_sub_measure, &d->btn_ctx[i]);
                obs_property_set_enabled(mp, false);
            }
        }
        obs_properties_add_group(props, "grp_sub", T_("GroupSubChannels"), OBS_GROUP_NORMAL, grp);
    }
    obs_properties_add_text(props, "about_info",
        "obs-delay-stream v2.0.0 | (C) 2026 Mazzn1987, Chigiri Tsutsumi | GPL 2.0+",
        OBS_TEXT_INFO);

    d->in_get_props.store(false);
    return props;
}

static void ds_get_defaults(obs_data_t* settings) {
    obs_data_set_default_bool  (settings, "enabled",               true);
    obs_data_set_default_bool  (settings, "ws_enabled",            true);
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
    }
}
