/*
 * obs-delay-stream  v1.7.0
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
#include "websocket-server.hpp"
#include "rtmp-prober.hpp"
#include "sync-flow.hpp"
#include "tunnel-manager.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-delay-stream", "en-US")

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

static std::string get_obs_stream_url() {
    return ""; // Set RTMP URL manually in the plugin panel
}

struct MeasureState {
    std::mutex    mtx;
    LatencyResult result;
    bool          measuring = false;
    bool          applied   = false;
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
    std::array<ChCtx, NUM_SUB_CH> tunnel_copy_ctx;
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
    std::string turl = d->tunnel.url();
    if (!turl.empty()) return d->tunnel.make_ch_url(sid, ch1);
    std::string ip = d->get_host_ip();
    if (ip.empty()) ip = d->auto_ip;
    if (ip.empty()) return "";
    char buf[256];
    snprintf(buf, sizeof(buf), "ws://%s:%d/%s/%d", ip.c_str(), WS_PORT, sid.c_str(), ch1);
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
static bool cb_tunnel_copy_url(obs_properties_t*, obs_property_t*, void*);
static bool cb_tunnel_copy_host(obs_properties_t*, obs_property_t*, void*);
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
    blog(LOG_INFO, "[obs-delay-stream] v1.7.0 loaded");
    return true;
}
void obs_module_unload(void) {}

static const char* ds_get_name(void*) { return "Delay Stream (obs-delay-stream)"; }

static void* ds_create(obs_data_t* settings, obs_source_t* source) {
    blog(LOG_INFO, "[obs-delay-stream] ds_create START");
    auto* d = new DelayStreamData();
    blog(LOG_INFO, "[obs-delay-stream] ds_create: DelayStreamData allocated at %p", (void*)d);
    d->context  = source;
    d->auto_ip  = get_local_ip();
    d->host_ip  = d->auto_ip;
    for (int i = 0; i < NUM_SUB_CH; ++i) {
        d->btn_ctx[i]         = { d, i };
        d->copy_ctx[i]        = { d, i };
        d->tunnel_copy_ctx[i] = { d, i };
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

    ds_update(d, settings);
    d->create_done.store(true);
    blog(LOG_INFO, "[obs-delay-stream] ds_create complete");
    return d;
}

static void ds_destroy(void* data) {
    auto* d = static_cast<DelayStreamData*>(data);

    // 1. コールバックをnull化（Use-After-Free 防止）
    d->flow.on_update       = nullptr;
    d->flow.on_ch_measured  = nullptr;
    d->flow.on_apply_sub    = nullptr;
    d->flow.on_apply_master = nullptr;
    d->rtmp.prober.on_result = nullptr;
    d->tunnel.on_url_ready  = nullptr;
    d->tunnel.on_error      = nullptr;
    d->tunnel.on_stopped    = nullptr;
    d->router.clear_callbacks();

    // 2. 各コンポーネントの停止（内部スレッドの join を含む）
    d->flow.reset();
    d->rtmp.prober.cancel();
    d->tunnel.stop();
    d->router.stop();

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
    d->sub[i].measure.measuring = true;
    d->router.start_measurement(i, PING_COUNT, PING_INTV_MS);
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
    return false;
}
static bool cb_tunnel_stop(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    d->tunnel.stop();
    request_properties_refresh(d);
    return false;
}
static bool cb_tunnel_copy_url(obs_properties_t*, obs_property_t*, void* priv) {
    auto* ctx = static_cast<ChCtx*>(priv);
    std::string url = ctx->d->tunnel.make_ch_url(ctx->d->get_stream_id(), ctx->ch + 1);
    if (url.empty()) return false;
    copy_to_clipboard(url);
    return false;
}
static bool cb_tunnel_copy_host(obs_properties_t*, obs_property_t*, void* priv) {
    auto* ctx = static_cast<ChCtx*>(priv);
    std::string url = ctx->d->tunnel.url();
    if (url.empty()) return false;
    std::string host = url;
    for (const char* pfx : {"wss://","ws://","https://","http://"}) {
        if (host.rfind(pfx, 0) == 0) { host = host.substr(strlen(pfx)); break; }
    }
    if (!host.empty() && host.back() == '/') host.pop_back();
    copy_to_clipboard(host);
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
        "Measures latency for all connected performers.\nSlowest-wins: all hear audio simultaneously.",
        OBS_TEXT_INFO);
    FlowPhase phase = d->flow.phase();
    FlowResult res  = d->flow.result();
    switch (phase) {
    case FlowPhase::Idle:
    case FlowPhase::Complete: {
        if (phase == FlowPhase::Complete)
            obs_properties_add_text(props, "flow_complete", "Sync Flow Complete!", OBS_TEXT_INFO);
        char buf[256] = "Connected: ";
        int nc = 0;
        for (int i = 0; i < NUM_SUB_CH; ++i)
            if (d->router.client_count(i) > 0) {
                char t[8]; snprintf(t, sizeof(t), "Ch%d ", i+1);
                strncat(buf, t, sizeof(buf)-strlen(buf)-1); ++nc;
            }
        if (nc == 0) strncat(buf, "(none)", sizeof(buf)-strlen(buf)-1);
        obs_properties_add_text(props, "flow_connected", buf, OBS_TEXT_INFO);
        if (nc > 0)
            obs_properties_add_button2(props, "flow_start_btn",
                "> Start Sync Flow", cb_flow_start, d);
        if (phase == FlowPhase::Complete)
            obs_properties_add_button2(props, "flow_reset_btn", "Reset", cb_flow_reset, d);
        break;
    }
    case FlowPhase::Step1_Measuring: {
        char buf[512];
        snprintf(buf, sizeof(buf), "Step1: Measuring... %d/%d done",
                 res.measured_count, res.connected_count);
        obs_properties_add_text(props, "flow_s1_prog", buf, OBS_TEXT_INFO);
        obs_properties_add_button2(props, "flow_cancel_s1m", "Cancel", cb_flow_reset, d);
        break;
    }
    case FlowPhase::Step1_Done: {
        char buf[1024];
        snprintf(buf, sizeof(buf), "Step1 Done - Baseline: %.1f ms", res.max_one_way_ms);
        for (int i = 0; i < NUM_SUB_CH; ++i) {
            if (!res.channels[i].connected) continue;
            char line[80];
            if (res.channels[i].measured)
                snprintf(line, sizeof(line), "\n  Ch%-2d %.1f ms -> +%.1f ms",
                         i+1, res.channels[i].one_way_ms, res.channels[i].proposed_delay);
            else
                snprintf(line, sizeof(line), "\n  Ch%-2d (failed)", i+1);
            strncat(buf, line, sizeof(buf)-strlen(buf)-1);
        }
        obs_properties_add_text(props, "flow_s1_result", buf, OBS_TEXT_INFO);
        obs_properties_add_button2(props, "flow_apply2_btn",
            "Apply Step2: Set sub-ch delays", cb_flow_apply_step2, d);
        obs_properties_add_button2(props, "flow_cancel_s1d", "Cancel", cb_flow_reset, d);
        break;
    }
    case FlowPhase::Step2_Applied: {
        char buf[256];
        snprintf(buf, sizeof(buf), "Step2 Done (%.1f ms) - Next: RTMP", res.max_one_way_ms);
        obs_properties_add_text(props, "flow_s2_done", buf, OBS_TEXT_INFO);
        obs_properties_add_button2(props, "flow_s3_btn",
            "> Step3: Measure RTMP", cb_flow_start_step3, d);
        obs_properties_add_button2(props, "flow_cancel_s2a", "Cancel", cb_flow_reset, d);
        break;
    }
    case FlowPhase::Step3_Measuring:
        obs_properties_add_text(props, "flow_s3_prog", "Step3: Measuring RTMP...", OBS_TEXT_INFO);
        obs_properties_add_button2(props, "flow_cancel_s3m", "Cancel", cb_flow_reset, d);
        break;
    case FlowPhase::Step3_Done: {
        char buf[512];
        if (res.rtmp_valid) {
            snprintf(buf, sizeof(buf),
                "Step3 Done\nRTMP: %.1f ms  Base: %.1f ms  Master: %.1f ms",
                res.rtmp_one_way_ms, res.max_one_way_ms, res.master_delay_ms);
            obs_properties_add_text(props, "flow_s3_result", buf, OBS_TEXT_INFO);
            char al[80];
            snprintf(al, sizeof(al), "Apply master delay %.1f ms", res.master_delay_ms);
            obs_properties_add_button2(props, "flow_apply3_btn", al, cb_flow_apply_step3, d);
        } else {
            snprintf(buf, sizeof(buf), "RTMP failed: %s", res.rtmp_error.c_str());
            obs_properties_add_text(props, "flow_s3_err", buf, OBS_TEXT_INFO);
            obs_properties_add_button2(props, "flow_retry3_btn", "Retry", cb_flow_start_step3, d);
        }
        obs_properties_add_button2(props, "flow_cancel_s3d", "Cancel", cb_flow_reset, d);
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

    // (1) ON/OFF
    obs_property_t* en_p = obs_properties_add_bool(props, "enabled",
        d->enabled.load() ? "Delay: ON" : "Delay: OFF (passthrough)");
    obs_property_t* ws_p = obs_properties_add_bool(props, "ws_enabled",
        d->ws_enabled.load() ? "WebSocket: ON" : "WebSocket: OFF");

    // WebSocket Server start/stop
    if (d->router_running.load()) {
        char ws_status[64];
        snprintf(ws_status, sizeof(ws_status), "WebSocket Server: Running (port %d)", WS_PORT);
        obs_properties_add_text(props, "ws_server_status", ws_status, OBS_TEXT_INFO);
        obs_properties_add_button2(props, "ws_server_stop_btn",
            "Stop WebSocket Server", cb_ws_server_stop, d);
    } else {
        obs_properties_add_text(props, "ws_server_status",
            "WebSocket Server: Stopped", OBS_TEXT_INFO);
        obs_properties_add_button2(props, "ws_server_start_btn",
            "Start WebSocket Server", cb_ws_server_start, d);
    }

    // (2) Stream ID / IP
    obs_properties_add_text(props, "sec_stream", "--- Stream ID / IP ---", OBS_TEXT_INFO);
    obs_property_t* sid_p = obs_properties_add_text(props, "stream_id", "Stream ID", OBS_TEXT_DEFAULT);
    { char info[128]; snprintf(info, sizeof(info), "Auto IP: %s", d->auto_ip.c_str());
      obs_properties_add_text(props, "auto_ip_info", info, OBS_TEXT_INFO); }
    obs_property_t* ip_p = obs_properties_add_text(props, "host_ip_manual", "IP override", OBS_TEXT_DEFAULT);

    // (3) Tunnel
    {
        TunnelState ts   = d->tunnel.state();
        std::string turl = d->tunnel.url();
        std::string terr = d->tunnel.error();
        obs_properties_add_text(props, "sec_tunnel", "--- Tunnel ---", OBS_TEXT_INFO);
        obs_property_t* type_p = obs_properties_add_list(props, "tunnel_type", "Service",
            OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
        obs_property_list_add_int(type_p, "ngrok (TCP)", 0);
        obs_property_list_add_int(type_p, "cloudflared (free)", 1);
        obs_properties_add_text(props, "ngrok_exe_path",       "ngrok.exe path", OBS_TEXT_DEFAULT);
        obs_properties_add_text(props, "ngrok_token",          "ngrok token",    OBS_TEXT_PASSWORD);
        obs_properties_add_text(props, "cloudflared_exe_path", "cloudflared.exe path", OBS_TEXT_DEFAULT);
        if (ts == TunnelState::Running || ts == TunnelState::Starting)
            obs_properties_add_button2(props, "tunnel_stop_btn",
                ts == TunnelState::Starting ? "Starting..." : "Stop Tunnel", cb_tunnel_stop, d);
        else
            obs_properties_add_button2(props, "tunnel_start_btn", "Start Tunnel", cb_tunnel_start, d);
        if (ts == TunnelState::Running && !turl.empty()) {
            char bi[256]; snprintf(bi, sizeof(bi), "URL: %s", turl.c_str());
            obs_properties_add_text(props, "tunnel_url_info", bi, OBS_TEXT_INFO);
            std::string sid_snap = d->get_stream_id();
            if (!sid_snap.empty()) {
                for (int i = 0; i < NUM_SUB_CH; ++i) {
                    d->tunnel_copy_ctx[i] = { d, i };
                    std::string ch_url = d->tunnel.make_ch_url(sid_snap, i+1);
                    char uk[32], ck[32], hk[32], ui[256], cl[32], hl[32];
                    snprintf(uk, sizeof(uk), "tch%d_url",  i);
                    snprintf(ck, sizeof(ck), "tch%d_copy", i);
                    snprintf(hk, sizeof(hk), "tch%d_host", i);
                    snprintf(ui, sizeof(ui), "Ch%d: %s [%zu]", i+1, ch_url.c_str(), d->router.client_count(i));
                    snprintf(cl, sizeof(cl), "Copy Ch%d URL",       i+1);
                    snprintf(hl, sizeof(hl), "Copy Host (Ch%d)",    i+1);
                    obs_properties_add_text(props, uk, ui, OBS_TEXT_INFO);
                    obs_properties_add_button2(props, ck, cl, cb_tunnel_copy_url,  &d->tunnel_copy_ctx[i]);
                    obs_properties_add_button2(props, hk, hl, cb_tunnel_copy_host, &d->tunnel_copy_ctx[i]);
                }
            }
        } else if (ts == TunnelState::Error && !terr.empty()) {
            char eb[256]; snprintf(eb, sizeof(eb), "Error: %s", terr.c_str());
            obs_properties_add_text(props, "tunnel_error", eb, OBS_TEXT_INFO);
        } else {
            obs_properties_add_text(props, "tunnel_idle", "Tunnel stopped.", OBS_TEXT_INFO);
        }
    }

    // (4) Sync Flow
    obs_properties_add_text(props, "sec_flow", "--- Sync Flow ---", OBS_TEXT_INFO);
    build_flow_panel(props, d);

    // (5) Master / RTMP
    obs_properties_add_text(props, "sec_master", "--- Master / RTMP ---", OBS_TEXT_INFO);
    obs_property_t* mp = obs_properties_add_float_slider(
        props, "master_delay_ms", "Master Delay (ms)", 0.0, MAX_DELAY_MS, 1.0);
    obs_property_float_set_suffix(mp, " ms");
    obs_properties_add_text(props, "rtmp_url_manual", "RTMP URL (manual)", OBS_TEXT_DEFAULT);
    obs_properties_add_button2(props, "rtmp_measure_btn",
        d->rtmp.prober.is_running() ? "Measuring..." : "Measure RTMP Latency",
        cb_rtmp_measure, d);
    {
        std::lock_guard<std::mutex> lk(d->rtmp.mtx);
        if (d->rtmp.result.valid) {
            char res[128];
            snprintf(res, sizeof(res), "RTT: %.1f ms / one-way: %.1f ms",
                     d->rtmp.result.avg_rtt_ms, d->rtmp.result.avg_one_way);
            obs_properties_add_text(props, "rtmp_result", res, OBS_TEXT_INFO);
            char al[64];
            snprintf(al, sizeof(al), "Apply (-> %.1f ms)", d->rtmp.result.avg_one_way);
            obs_properties_add_button2(props, "rtmp_apply_btn", al, cb_rtmp_apply, d);
        } else {
            obs_properties_add_text(props, "rtmp_no_result", "(not measured)", OBS_TEXT_INFO);
        }
    }

    // (5.5) Sub-CH Global Offset
    obs_properties_add_text(props, "sec_offset",
        "--- Sub-CH Global Offset (RTMP sync adjustment) ---", OBS_TEXT_INFO);
    {
        char offset_info[256];
        snprintf(offset_info, sizeof(offset_info),
            "Add extra delay to ALL sub-ch after Sync Flow.\n"
            "Use this to compensate for RTMP decode/playback latency.\n"
            "Current offset: %.1f ms  (each ch base + offset >= 0)", d->sub_offset_ms);
        obs_properties_add_text(props, "sub_offset_info", offset_info, OBS_TEXT_INFO);
    }
    obs_property_t* op = obs_properties_add_float_slider(
        props, "sub_offset_ms",
        "Sub-CH Global Offset (ms)", -2000.0, 5000.0, 10.0);
    obs_property_float_set_suffix(op, " ms");

    // (6) Sub channels
    obs_properties_add_text(props, "sec_sub", "--- Per-Performer Channels ---", OBS_TEXT_INFO);
    for (int i = 0; i < NUM_SUB_CH; ++i) {
        d->btn_ctx[i]  = { d, i };
        d->copy_ctx[i] = { d, i };
        char dk[32], uk[32], mk[32], ak[32], rk[32], ck[32];
        char dn[64], us[256];
        snprintf(dk, sizeof(dk), "sub%d_delay_ms",    i);
        snprintf(dn, sizeof(dn), "Ch%d Delay (ms)",   i+1);
        snprintf(uk, sizeof(uk), "sub%d_url",         i);
        snprintf(mk, sizeof(mk), "sub%d_meas",        i);
        snprintf(ak, sizeof(ak), "sub%d_apply",       i);
        snprintf(rk, sizeof(rk), "sub%d_result",      i);
        snprintf(ck, sizeof(ck), "sub%d_copy",        i);
        std::string url = make_sub_url(d, i+1);
        size_t nc = d->router.client_count(i);
        if (url.empty()) snprintf(us, sizeof(us), "Ch%d [%zu conn]", i+1, nc);
        else             snprintf(us, sizeof(us), "Ch%d: %s [%zu]",  i+1, url.c_str(), nc);
        obs_properties_add_text(props, uk, us, OBS_TEXT_INFO);
        obs_property_t* sp = obs_properties_add_float_slider(props, dk, dn, 0.0, MAX_DELAY_MS, 1.0);
        obs_property_float_set_suffix(sp, " ms");
        if (!url.empty()) {
            char cl[32]; snprintf(cl, sizeof(cl), "Copy Ch%d URL", i+1);
            obs_properties_add_button2(props, ck, cl, cb_sub_copy_url, &d->copy_ctx[i]);
        }
        if (nc > 0) {
            MeasureState& ms = d->sub[i].measure;
            std::lock_guard<std::mutex> lk(ms.mtx);
            if (ms.measuring) {
                obs_properties_add_text(props, rk, "Measuring...", OBS_TEXT_INFO);
            } else if (ms.result.valid) {
                char rs[128];
                snprintf(rs, sizeof(rs), "RTT:%.1f one-way:%.1f ms",
                         ms.result.avg_rtt_ms, ms.result.avg_one_way);
                obs_properties_add_text(props, rk, rs, OBS_TEXT_INFO);
                char al[64]; snprintf(al, sizeof(al), "Apply (-> %.1f ms)", ms.result.avg_one_way);
                obs_properties_add_button2(props, ak, al, cb_sub_apply, &d->btn_ctx[i]);
            } else {
                obs_properties_add_button2(props, mk, "Measure Latency",
                    cb_sub_measure, &d->btn_ctx[i]);
            }
        } else {
            obs_properties_add_text(props, rk, "(connect performer first)", OBS_TEXT_INFO);
        }
    }
    d->in_get_props.store(false);
    return props;
}

static void ds_get_defaults(obs_data_t* settings) {
    obs_data_set_default_bool  (settings, "enabled",               true);
    obs_data_set_default_bool  (settings, "ws_enabled",            true);
    obs_data_set_default_string(settings, "stream_id",             "");
    obs_data_set_default_string(settings, "host_ip_manual",        "");
    obs_data_set_default_double(settings, "master_delay_ms",       0.0);
    obs_data_set_default_double(settings, "sub_offset_ms",         0.0);
    obs_data_set_default_string(settings, "rtmp_url_manual",       "");
    obs_data_set_default_int   (settings, "tunnel_type",           1);
    obs_data_set_default_string(settings, "ngrok_exe_path",        "C:\\ngrok\\ngrok.exe");
    obs_data_set_default_string(settings, "ngrok_token",           "");
    obs_data_set_default_string(settings, "cloudflared_exe_path",  "C:\\cloudflared\\cloudflared.exe");
    for (int i = 0; i < NUM_SUB_CH; ++i) {
        char key[32]; snprintf(key, sizeof(key), "sub%d_delay_ms", i);
        obs_data_set_default_double(settings, key, 0.0);
    }
}
