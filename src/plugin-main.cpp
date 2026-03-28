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
#include <psapi.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "Psapi.lib")
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
#include "constants.hpp"
#include "delay-filter.hpp"
#include "receiver_index_html.hpp"
#include "websocket-server.hpp"
#include "rtmp-prober.hpp"
#include "sync-flow.hpp"
#include "tunnel-manager.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-delay-stream", "ja-JP")
#define T_(s) obs_module_text(s)

// OBSプロセス内では本フィルタを1インスタンスだけ許可する。
static std::atomic<bool> g_delay_stream_filter_exists{false};
static constexpr float   SUB_ADJUST_MIN_MS = -500.0f;
static constexpr float   SUB_ADJUST_MAX_MS = 500.0f;
static std::atomic<uint64_t> g_props_refresh_seq{0};

// 前後の空白を削除した文字列を返す。
static std::string trim_copy(std::string s) {
    const char* ws = " \t\r\n";
    const size_t begin = s.find_first_not_of(ws);
    if (begin == std::string::npos) return "";
    const size_t end = s.find_last_not_of(ws);
    return s.substr(begin, end - begin + 1);
}

// RTMP/RTMPSスキームのURLならそのまま返す。対象外は空文字。
static std::string normalize_rtmp_url_candidate(const char* raw) {
    if (!raw || !*raw) return "";
    std::string url = trim_copy(raw);
    if (url.empty()) return "";
    if (_strnicmp(url.c_str(), "rtmp://", 7) == 0) return url;
    if (_strnicmp(url.c_str(), "rtmps://", 8) == 0) return url;
    if (url.find("://") != std::string::npos) return "";
    if (_stricmp(url.c_str(), "auto") == 0) return "";
    if (_stricmp(url.c_str(), "default") == 0) return "";
    if (url.find_first_of(" \t\r\n") != std::string::npos) return "";
    return "rtmp://" + url;
}

// RTMPサーバーURLとストリームキーを結合する（例: rtmp://host/live + key）。
static std::string join_rtmp_url_and_stream_key(const std::string& base_url,
                                                const std::string& raw_key) {
    std::string base = trim_copy(base_url);
    std::string key  = trim_copy(raw_key);
    if (base.empty() || key.empty()) return base;

    // "rtmp://.../key" が渡された場合はそのまま採用する。
    if (_strnicmp(key.c_str(), "rtmp://", 7) == 0 ||
        _strnicmp(key.c_str(), "rtmps://", 8) == 0) {
        return normalize_rtmp_url_candidate(key.c_str());
    }

    while (!key.empty() && key.front() == '/') key.erase(key.begin());
    if (key.empty()) return base;

    // 既に末尾が同じキーなら重複連結しない。
    if (base.size() >= key.size() &&
        base.compare(base.size() - key.size(), key.size(), key) == 0) {
        if (base.size() == key.size() || base[base.size() - key.size() - 1] == '/') {
            return base;
        }
    }

    if (!base.empty() && base.back() != '/') base.push_back('/');
    return base + key;
}

// 既にロード済みのOBS関連モジュールからシンボルを取得する。
template <typename Fn>
static Fn find_obs_symbol(const char* symbol_name) {
    if (!symbol_name || !*symbol_name) return nullptr;
    const char* modules[] = {
        "obs-frontend-api.dll",
        "obs-frontend-api64.dll",
        "obs-frontend-api",
        "obs64.exe",
        "obs64",
        "obs.dll",
        "libobs.dll",
        "libobs",
        nullptr
    };
    for (int i = 0; modules[i]; ++i) {
        HMODULE mod = GetModuleHandleA(modules[i]);
        if (!mod) continue;
        FARPROC p = GetProcAddress(mod, symbol_name);
        if (p) return reinterpret_cast<Fn>(p);
    }
    for (int i = 0; modules[i]; ++i) {
        HMODULE mod = LoadLibraryA(modules[i]);
        if (!mod) continue;
        FARPROC p = GetProcAddress(mod, symbol_name);
        if (p) return reinterpret_cast<Fn>(p);
    }

    // モジュール名差異に備えて、ロード済み全モジュールを総当たりする。
    HMODULE mods[1024];
    DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        const DWORD count = needed / sizeof(HMODULE);
        for (DWORD i = 0; i < count; ++i) {
            FARPROC p = GetProcAddress(mods[i], symbol_name);
            if (p) return reinterpret_cast<Fn>(p);
        }
    }
    return nullptr;
}

// OBS本体の source removed 判定API（存在する場合のみ）を呼ぶ。
static bool is_obs_source_removed(obs_source_t* source) {
    using source_removed_fn = bool (*)(obs_source_t*);
    static source_removed_fn fn =
        find_obs_symbol<source_removed_fn>("obs_source_removed");
    if (!source || !fn) return false;
    return fn(source);
}

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

// 0始まりのカウンター値を A, B, ... Z, AA, AB, ... 形式へ変換する。
static std::string make_alpha_counter_label(int index) {
    if (index < 0) index = 0;
    std::string out;
    for (int n = index; n >= 0; n = (n / 26) - 1) {
        out.push_back(static_cast<char>('A' + (n % 26)));
    }
    std::reverse(out.begin(), out.end());
    return out;
}

// チャンネル追加時の既定名（例: 演者A, 演者B ...）を生成する。
static std::string make_default_sub_memo(int counter) {
    const char* prefix = T_("SubDefaultMemoPrefix");
    if (!prefix || !*prefix) prefix = "Performer";
    return std::string(prefix) + make_alpha_counter_label(counter);
}

// チャンネル数を有効範囲(1..MAX_SUB_CH)に丸める。
static int clamp_sub_ch_count(int v) {
    if (v < 1) return 1;
    if (v > MAX_SUB_CH) return MAX_SUB_CH;
    return v;
}

static int normalize_opus_sample_rate(int v) {
    switch (v) {
    case 0:
    case 8000:
    case 12000:
    case 16000:
    case 24000:
    case 48000:
        return v;
    default:
        return 0;
    }
}

static int normalize_quantization_bits(int v) {
    switch (v) {
    case 8:
    case 16:
        return v;
    default:
        return 16;
    }
}

static int normalize_pcm_downsample_ratio(int v) {
    switch (v) {
    case 1: case 2: case 4: return v;
    default: return 1;
    }
}

// ファイル全体を文字列として読み込む。
static std::string read_file_to_string(const char* path) {
    if (!path || !*path) return "";
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return "";
    return std::string((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());
}

static constexpr const char* kReceiverBuildTimestamp = __DATE__ " " __TIME__;

// 文字列中のトークンをすべて置換する。
static void replace_all(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.length(), to);
        pos += to.length();
    }
}

// receiver/index.html をモジュール配下→相対パス→埋め込みの順で読み込み、
// デバッグ用トークンを埋め込む。
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
    replace_all(html, "@PROJECT_VERSION@", PLUGIN_VERSION);
    replace_all(html, "@BUILD_TIMESTAMP@", kReceiverBuildTimestamp);
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

// OBS本体の配信サービス設定からRTMP URLを取得する。
static std::string get_obs_stream_url() {
    using get_streaming_service_fn = obs_service_t* (*)();
    using service_get_settings_fn = obs_data_t* (*)(obs_service_t*);
    using service_get_ref_fn = obs_service_t* (*)(obs_service_t*);
    using service_addref_fn = void (*)(obs_service_t*);
    using service_get_url_fn = const char* (*)(obs_service_t*);
    using service_release_fn = void (*)(obs_service_t*);

    auto get_streaming_service =
        find_obs_symbol<get_streaming_service_fn>("obs_frontend_get_streaming_service");
    auto service_get_settings =
        find_obs_symbol<service_get_settings_fn>("obs_service_get_settings");
    auto service_get_ref =
        find_obs_symbol<service_get_ref_fn>("obs_service_get_ref");
    auto service_addref =
        find_obs_symbol<service_addref_fn>("obs_service_addref");
    auto service_get_url =
        find_obs_symbol<service_get_url_fn>("obs_service_get_url");
    auto service_release =
        find_obs_symbol<service_release_fn>("obs_service_release");
    if (!get_streaming_service || !service_get_settings) return "";

    // OBS 32系では frontend_get_streaming_service は借用参照を返すため、
    // 直接 release しない。必要ならここで明示的に参照を取得して使う。
    obs_service_t* borrowed = get_streaming_service();
    if (!borrowed) return "";

    obs_service_t* owned = nullptr;
    if (service_get_ref) {
        owned = service_get_ref(borrowed);
    } else if (service_addref && service_release) {
        service_addref(borrowed);
        owned = borrowed;
    }
    obs_service_t* service = owned ? owned : borrowed;

    std::string url;
    std::string stream_key;
    if (service_get_url) {
        url = normalize_rtmp_url_candidate(service_get_url(service));
    }

    obs_data_t* settings = service_get_settings(service);
    if (settings) {
        if (url.empty()) {
            const char* keys[] = {
                "server",
                "url",
                "ingest_url",
                "server_url",
                "rtmp_url",
                nullptr
            };
            for (int i = 0; keys[i]; ++i) {
                url = normalize_rtmp_url_candidate(obs_data_get_string(settings, keys[i]));
                if (!url.empty()) break;
            }
        }

        const char* key_keys[] = {
            "key",
            "stream_key",
            "streamkey",
            "play_path",
            "path",
            nullptr
        };
        for (int i = 0; key_keys[i]; ++i) {
            stream_key = trim_copy(obs_data_get_string(settings, key_keys[i]));
            if (!stream_key.empty()) break;
        }
        obs_data_release(settings);
    }

    if (!url.empty() && !stream_key.empty()) {
        url = join_rtmp_url_and_stream_key(url, stream_key);
    }

    if (owned && service_release) {
        service_release(owned);
    }
    return url;
}

// RTMP URL自動取得ONのとき、OBS配信設定のURLで補完する。
static void maybe_autofill_rtmp_url(obs_data_t* settings, bool force_refresh) {
    if (!settings) return;
    if (!obs_data_get_bool(settings, "rtmp_url_auto")) return;
    std::string configured = trim_copy(obs_data_get_string(settings, "rtmp_url"));
    if (!force_refresh && !configured.empty()) return;
    std::string auto_url = get_obs_stream_url();
    if (auto_url.empty()) return;
    if (configured == auto_url) return;
    obs_data_set_string(settings, "rtmp_url", auto_url.c_str());
}

// ソース設定を取得してRTMP URL自動補完を試みる。
static void maybe_autofill_rtmp_url_from_source(obs_source_t* source, bool force_refresh) {
    if (!source) return;
    obs_data_t* s = obs_source_get_settings(source);
    if (!s) return;
    maybe_autofill_rtmp_url(s, force_refresh);
    obs_data_release(s);
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
    bool is_duplicate_instance = false;
    bool owns_singleton_slot   = false;
    std::atomic<bool> destroying{false};
    std::atomic<bool> enabled{true};
    std::atomic<bool> ws_send_enabled{true};
    std::shared_ptr<std::atomic<bool>> life_token =
        std::make_shared<std::atomic<bool>>(true);
    mutable std::mutex stream_id_mtx;  // protects stream_id, host_ip
    std::string   stream_id;
    std::string   host_ip;
    std::string   auto_ip;
    std::atomic<int> ws_port{WS_PORT};
    float         master_delay_ms = 0.0f;
    float         sub_offset_ms   = 0.0f; // global offset added to all channel after Sync Flow
    int           sub_ch_count    = 1; // active channel count (1..MAX_SUB_CH)
    DelayBuffer   master_buf;
    RtmpMeasureState rtmp;
    StreamRouter  router;
    std::atomic<bool> router_running{false};
    std::array<ChCtx, MAX_SUB_CH> btn_ctx;
    struct SubChannel {
        float        delay_ms = 0.0f;
        float        adjust_ms = 0.0f;
        DelayBuffer  buf;
        MeasureState measure;
    };
    std::array<SubChannel, MAX_SUB_CH> sub;
    SyncFlow      flow;
    TunnelManager tunnel;
    uint32_t      sample_rate = 48000;
    uint32_t      channels    = 2;
    bool          initialized = false;
    std::atomic<bool> create_done{false}; // set true after ds_create completes
    std::atomic<bool> in_get_props{false}; // true while ds_get_properties is running
    std::atomic<bool> sid_autofill_guard{false};
    std::atomic<bool> detail_mode{false};
    std::atomic<bool> rtmp_url_auto{true};
    bool prev_stream_id_has_user_value = false;
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

// チャンネルの最終遅延値（base + adjust + global offset）を算出する。
static float calc_effective_sub_delay_value_ms(const DelayStreamData* d,
                                               float base_delay_ms,
                                               float adjust_ms) {
    if (!d) return 0.0f;
    float effective = base_delay_ms + adjust_ms + d->sub_offset_ms;
    if (effective < 0.0f) effective = 0.0f;
    return effective;
}

// チャンネルの最終遅延値（base + adjust + global offset）を算出して、無効時は0にする。
static uint32_t calc_effective_sub_delay_ms(const DelayStreamData* d,
                                            float base_delay_ms,
                                            float adjust_ms) {
    if (!d || !d->enabled.load(std::memory_order_relaxed)) return 0;
    return static_cast<uint32_t>(
        calc_effective_sub_delay_value_ms(d, base_delay_ms, adjust_ms));
}

// 指定チャンネルのバッファ遅延を現在設定へ反映する。
static void apply_sub_delay_to_buffer(DelayStreamData* d, int ch) {
    if (!d || ch < 0 || ch >= MAX_SUB_CH) return;
    d->sub[ch].buf.set_delay_ms(
        calc_effective_sub_delay_ms(d, d->sub[ch].delay_ms, d->sub[ch].adjust_ms));
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
    std::string url;
    obs_data_t* s = obs_source_get_settings(d->context);
    if (!s) return "";
    bool auto_mode = obs_data_get_bool(s, "rtmp_url_auto");
    if (auto_mode) {
        url = get_obs_stream_url();
    }
    if (url.empty()) {
        const char* configured = obs_data_get_string(s, "rtmp_url");
        if (configured && *configured) url = configured;
    }
    obs_data_release(s);
    return url;
}

// cloudflaredの自動検出結果があれば設定へ反映する。
static void maybe_fill_cloudflared_path_from_auto(DelayStreamData* d) {
    if (!d || !d->context) return;
    obs_data_t* s = obs_source_get_settings(d->context);
    const char* cur = obs_data_get_string(s, "cloudflared_exe_path");
    if (cur && _stricmp(cur, "auto") == 0) {
        obs_data_set_string(s, "cloudflared_exe_path", "");
        cur = "";
    }
    bool is_auto = (!cur || !*cur);
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

struct PropsRefreshCtx {
    obs_source_t* source = nullptr;
    uint64_t      seq = 0;
    const char*   reason = nullptr;
};
static std::mutex              g_props_refresh_pending_mtx;
static std::set<obs_source_t*> g_props_refresh_pending_sources;
static std::set<obs_source_t*> g_props_refresh_blocked_sources;

// UIスレッド側でプロパティ再構築を実行する。
static void do_request_properties_refresh_ui(void* p) {
    auto* ctx = static_cast<PropsRefreshCtx*>(p);
    if (!ctx || !ctx->source) {
        delete ctx;
        return;
    }
    bool should_update = true;
    {
        std::lock_guard<std::mutex> lk(g_props_refresh_pending_mtx);
        g_props_refresh_pending_sources.erase(ctx->source);
        if (g_props_refresh_blocked_sources.find(ctx->source) !=
            g_props_refresh_blocked_sources.end()) {
            should_update = false;
        }
    }
    if (should_update && is_obs_source_removed(ctx->source)) {
        should_update = false;
    }
    if (should_update) {
        blog(LOG_INFO, "[obs-delay-stream] props_refresh exec seq=%llu reason=%s",
             (unsigned long long)ctx->seq, ctx->reason ? ctx->reason : "(null)");
        obs_source_update_properties(ctx->source);
    } else {
        blog(LOG_INFO, "[obs-delay-stream] props_refresh skip seq=%llu reason=%s",
             (unsigned long long)ctx->seq, ctx->reason ? ctx->reason : "(null)");
    }
    obs_source_release(ctx->source);
    delete ctx;
}

// UIスレッド再入を避けるため、いったん別スレッドへ退避してからUIへ戻す。
static void bounce_request_properties_refresh(void* p) {
    obs_queue_task(OBS_TASK_UI, do_request_properties_refresh_ui, p, false);
}

// プロパティUIの再描画を依頼する。
static void request_properties_refresh(DelayStreamData* d, const char* reason = nullptr) {
    if (!d || !d->context || !d->create_done.load()) return;
    if (d->destroying.load(std::memory_order_acquire)) return;
    if (is_obs_source_removed(d->context)) return;
    if (d->in_get_props.load()) return; // prevent re-entry

    uint64_t seq = g_props_refresh_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    auto* ctx = new PropsRefreshCtx();
    ctx->seq = seq;
    ctx->reason = reason ? reason : "unspecified";
    ctx->source = obs_source_get_ref(d->context);
    if (!ctx->source) {
        delete ctx;
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_props_refresh_pending_mtx);
        if (g_props_refresh_blocked_sources.find(ctx->source) !=
            g_props_refresh_blocked_sources.end()) {
            blog(LOG_INFO, "[obs-delay-stream] props_refresh drop(blocked) seq=%llu reason=%s",
                 (unsigned long long)ctx->seq, ctx->reason ? ctx->reason : "(null)");
            obs_source_release(ctx->source);
            delete ctx;
            return;
        }
        if (g_props_refresh_pending_sources.find(ctx->source) !=
            g_props_refresh_pending_sources.end()) {
            blog(LOG_INFO, "[obs-delay-stream] props_refresh drop(pending) seq=%llu reason=%s",
                 (unsigned long long)ctx->seq, ctx->reason ? ctx->reason : "(null)");
            obs_source_release(ctx->source);
            delete ctx;
            return;
        }
        g_props_refresh_pending_sources.insert(ctx->source);
    }

    if (obs_in_task_thread(OBS_TASK_UI)) {
        // 直接呼ぶと ControlChanged 中に再入しうるため、別タスク経由で遅延実行する。
        blog(LOG_INFO, "[obs-delay-stream] props_refresh queue(seq=%llu reason=%s via=graphics->ui)",
             (unsigned long long)ctx->seq, ctx->reason ? ctx->reason : "(null)");
        obs_queue_task(OBS_TASK_GRAPHICS, bounce_request_properties_refresh, ctx, false);
    } else {
        blog(LOG_INFO, "[obs-delay-stream] props_refresh queue(seq=%llu reason=%s via=ui)",
             (unsigned long long)ctx->seq, ctx->reason ? ctx->reason : "(null)");
        obs_queue_task(OBS_TASK_UI, do_request_properties_refresh_ui, ctx, false);
    }
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
        int ws_port = d->ws_port.load(std::memory_order_relaxed);
        char buf[256];
        snprintf(buf, sizeof(buf), "http://%s:%d", ip.c_str(), ws_port);
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
static bool cb_sub_adjust_changed(void*, obs_properties_t*, obs_property_t*, obs_data_t*);
static bool cb_sub_copy_all(obs_properties_t*, obs_property_t*, void*);
static bool cb_sub_add(obs_properties_t*, obs_property_t*, void*);
static bool cb_sub_remove(obs_properties_t*, obs_property_t*, void*);
static bool cb_rtmp_measure(obs_properties_t*, obs_property_t*, void*);
static bool cb_rtmp_apply(obs_properties_t*, obs_property_t*, void*);
static bool cb_rtmp_url_auto_changed(void*, obs_properties_t*, obs_property_t*, obs_data_t*);
static bool cb_tunnel_start(obs_properties_t*, obs_property_t*, void*);
static bool cb_tunnel_stop(obs_properties_t*, obs_property_t*, void*);
static bool cb_flow_start(obs_properties_t*, obs_property_t*, void*);
static bool cb_flow_retry_failed(obs_properties_t*, obs_property_t*, void*);
static bool cb_flow_start_step3(obs_properties_t*, obs_property_t*, void*);
static bool cb_flow_apply_step3(obs_properties_t*, obs_property_t*, void*);
static bool cb_flow_reset(obs_properties_t*, obs_property_t*, void*);
static bool cb_enabled_changed(void*, obs_properties_t*, obs_property_t*, obs_data_t*);
static bool cb_audio_codec_changed(void*, obs_properties_t*, obs_property_t*, obs_data_t*);
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
    bool already_exists =
        g_delay_stream_filter_exists.exchange(true, std::memory_order_acq_rel);
    blog(LOG_INFO, "[obs-delay-stream] ds_create START");
    auto* d = new DelayStreamData();
    blog(LOG_INFO, "[obs-delay-stream] ds_create: DelayStreamData allocated at %p", (void*)d);
    d->is_duplicate_instance = already_exists;
    d->owns_singleton_slot   = !already_exists;
    d->context  = source;
    {
        std::lock_guard<std::mutex> lk(g_props_refresh_pending_mtx);
        g_props_refresh_blocked_sources.erase(source);
        g_props_refresh_pending_sources.erase(source);
    }
    maybe_autofill_rtmp_url(settings, false);
    d->rtmp_url_auto.store(obs_data_get_bool(settings, "rtmp_url_auto"), std::memory_order_relaxed);
    if (d->is_duplicate_instance) {
        blog(LOG_WARNING, "[obs-delay-stream] duplicate filter instance created as warning-only");
        d->create_done.store(true);
        return d;
    }
    {
        auto html = load_receiver_index_html();
        if (html.empty()) {
            blog(LOG_WARNING, "[obs-delay-stream] receiver/index.html not found; HTTP top page disabled");
        } else {
            blog(LOG_INFO, "[obs-delay-stream] receiver html loaded: build=%s",
                 kReceiverBuildTimestamp);
        }
        d->router.set_http_index_html(std::move(html));
        auto root = get_receiver_root_dir();
        if (!root.empty()) d->router.set_http_root_dir(std::move(root));
    }
    d->auto_ip  = get_local_ip();
    d->host_ip  = d->auto_ip;
    for (int i = 0; i < MAX_SUB_CH; ++i) {
        d->btn_ctx[i]         = { d, i };
    }
    d->flow.on_update      = [d]() { request_properties_refresh(d, "flow.on_update"); };
    d->flow.on_ch_measured = [d](int, LatencyResult) {
        request_properties_refresh(d, "flow.on_ch_measured");
    };
    d->flow.on_apply_master = [d](double ms) {
        struct Ctx {
            std::weak_ptr<std::atomic<bool>> life_token;
            DelayStreamData* d;
            double ms;
        };
        auto* c = new Ctx{d->life_token, d, ms};
        obs_queue_task(OBS_TASK_UI, [](void* p){
            auto* c = static_cast<Ctx*>(p);
            auto life = c->life_token.lock();
            if (!life || !life->load(std::memory_order_acquire)) {
                delete c;
                return;
            }
            write_master_delay(c->d, c->ms);
            request_properties_refresh(c->d, "flow.on_apply_master");
            delete c;
        }, c, false);
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
    d->tunnel.on_download_state = [d](bool) {
        request_properties_refresh(d, "tunnel.on_download_state");
    };
    d->router.on_conn_change = [d](const std::string& sid, int, size_t) {
        if (sid == d->get_stream_id()) request_properties_refresh(d, "router.on_conn_change");
    };
    d->router.on_any_latency_result = [d](const std::string& sid, int ch, LatencyResult r) {
        if (sid != d->get_stream_id()) return;
        if (ch < 0 || ch >= MAX_SUB_CH) return;
        auto& ms = d->sub[ch].measure;
        bool should_apply = r.valid;
        {
            std::lock_guard<std::mutex> lk(ms.mtx);
            ms.result = r;
            ms.measuring = false;
            ms.applied = should_apply;
            ms.last_error = r.valid ? "" : T_("MeasureFailed");
        }
        if (should_apply) {
            struct Ctx {
                std::weak_ptr<std::atomic<bool>> life_token;
                DelayStreamData* d;
                int ch;
                double ms;
            };
            auto* c = new Ctx{d->life_token, d, ch, r.avg_one_way};
            obs_queue_task(OBS_TASK_UI, [](void* p){
                auto* c = static_cast<Ctx*>(p);
                auto life = c->life_token.lock();
                if (!life || !life->load(std::memory_order_acquire)) {
                    delete c;
                    return;
                }
                apply_sub_delay(c->d, c->ch, c->ms);
                request_properties_refresh(c->d, "router.on_any_latency_result.apply");
                delete c;
            }, c, false);
        } else {
            request_properties_refresh(d, "router.on_any_latency_result.invalid");
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
    if (!d) return;
    d->destroying.store(true, std::memory_order_release);
    if (d->life_token) {
        d->life_token->store(false, std::memory_order_release);
        d->life_token.reset();
    }
    if (d->context) {
        std::lock_guard<std::mutex> lk(g_props_refresh_pending_mtx);
        g_props_refresh_blocked_sources.insert(d->context);
        g_props_refresh_pending_sources.erase(d->context);
    }
    bool release_singleton_slot = d->owns_singleton_slot;
    // 1. 各コンポーネントの停止（内部スレッドの join を含む）
    if (!d->is_duplicate_instance) {
        d->flow.reset();
        d->rtmp.prober.cancel();
        d->tunnel.stop();
        d->router.stop();

        // 2. コールバックをnull化（停止後なので競合しない）
        d->flow.on_update       = nullptr;
        d->flow.on_ch_measured  = nullptr;
        d->flow.on_apply_master = nullptr;
        d->rtmp.prober.on_result = nullptr;
        d->tunnel.on_url_ready  = nullptr;
        d->tunnel.on_error      = nullptr;
        d->tunnel.on_stopped    = nullptr;
        d->tunnel.on_download_state = nullptr;
        d->router.clear_callbacks();
    }

    delete d;
    if (release_singleton_slot) {
        g_delay_stream_filter_exists.store(false, std::memory_order_release);
    }
}

// サンプルレート/チャンネル変更時に内部バッファを再初期化する。
static void ensure_init(DelayStreamData* d, uint32_t sr, uint32_t ch) {
    if (d->initialized && d->sample_rate == sr && d->channels == ch) return;
    d->sample_rate = sr; d->channels = ch;
    d->master_buf.init(sr, ch);
    d->master_buf.set_delay_ms((uint32_t)d->master_delay_ms);
    for (int i = 0; i < MAX_SUB_CH; ++i) {
        d->sub[i].buf.init(sr, ch);
        apply_sub_delay_to_buffer(d, i);
    }
    d->work_buf.resize(65536 * ch, 0.0f);
    d->initialized = true;
}

// OBS設定値を内部状態へ同期する。
static void ds_update(void* data, obs_data_t* settings) {
    auto* d = static_cast<DelayStreamData*>(data);
    if (d->is_duplicate_instance) {
        d->enabled.store(false, std::memory_order_relaxed);
        d->ws_send_enabled.store(false, std::memory_order_relaxed);
        d->detail_mode.store(false, std::memory_order_relaxed);
        d->prev_stream_id_has_user_value = obs_data_has_user_value(settings, "stream_id");
        return;
    }
    bool stream_id_has_user_value = obs_data_has_user_value(settings, "stream_id");
    bool reset_to_defaults =
        d->create_done.load(std::memory_order_relaxed) &&
        d->prev_stream_id_has_user_value &&
        !stream_id_has_user_value;

    if (reset_to_defaults) {
        if (d->router_running.load(std::memory_order_relaxed)) {
            d->router.stop();
            d->router_running.store(false, std::memory_order_relaxed);
            blog(LOG_INFO, "[obs-delay-stream] WebSocket server stopped by defaults reset");
        }
        TunnelState ts = d->tunnel.state();
        if (ts == TunnelState::Starting || ts == TunnelState::Running) {
            d->tunnel.stop();
            blog(LOG_INFO, "[obs-delay-stream] Tunnel stopped by defaults reset");
        }
    }

    bool detail_new = obs_data_get_bool(settings, "detail_mode");
    bool detail_old = d->detail_mode.exchange(detail_new, std::memory_order_relaxed);
    bool detail_changed = (detail_old != detail_new);
    bool delay_disable = obs_data_get_bool(settings, "delay_disable");
    d->enabled.store(!delay_disable);
    bool paused = obs_data_get_bool(settings, "ws_send_paused");
    d->ws_send_enabled.store(!paused);
    {
        int current = d->sub_ch_count;
        bool has_sub_ch_count = obs_data_has_user_value(settings, "sub_ch_count");
        int raw_v = has_sub_ch_count ? (int)obs_data_get_int(settings, "sub_ch_count") : current;
        int v = raw_v;
        if (v <= 0) v = (current > 0) ? current : 1;
        int clamped = clamp_sub_ch_count(v);
        if (!has_sub_ch_count || clamped != v) {
            obs_data_set_int(settings, "sub_ch_count", clamped);
        }
        bool changed = (clamped != current);
        if (changed || !has_sub_ch_count || raw_v != clamped) {
            blog(LOG_INFO,
                 "[obs-delay-stream] ds_update sub_ch_count current=%d has_user=%d raw=%d normalized=%d clamped=%d",
                 current, has_sub_ch_count ? 1 : 0, raw_v, v, clamped);
        }
        d->sub_ch_count = clamped;
        d->router.set_active_channels(clamped);
        d->flow.set_active_channels(clamped);
        if (changed) d->flow.reset();
    }
    int audio_codec = (int)obs_data_get_int(settings, "audio_codec");
    d->router.set_audio_codec(audio_codec);
    {
        int bitrate = (int)obs_data_get_int(settings, "opus_bitrate_kbps");
        if (bitrate < 6) {
            bitrate = 6;
            obs_data_set_int(settings, "opus_bitrate_kbps", bitrate);
        } else if (bitrate > 510) {
            bitrate = 510;
            obs_data_set_int(settings, "opus_bitrate_kbps", bitrate);
        }
        d->router.set_opus_bitrate_kbps(bitrate);
    }
    {
        int sample_rate = normalize_opus_sample_rate(
            (int)obs_data_get_int(settings, "opus_sample_rate"));
        if (sample_rate != (int)obs_data_get_int(settings, "opus_sample_rate")) {
            obs_data_set_int(settings, "opus_sample_rate", sample_rate);
        }
        d->router.set_opus_target_sample_rate(sample_rate);
    }
    {
        int quant_bits = normalize_quantization_bits(
            (int)obs_data_get_int(settings, "quantization_bits"));
        if (quant_bits != (int)obs_data_get_int(settings, "quantization_bits")) {
            obs_data_set_int(settings, "quantization_bits", quant_bits);
        }
        d->router.set_audio_quantization_bits(quant_bits);
    }
    d->router.set_audio_mono(obs_data_get_bool(settings, "audio_mono"));
    {
        int ratio = normalize_pcm_downsample_ratio(
            (int)obs_data_get_int(settings, "pcm_downsample_ratio"));
        if (ratio != (int)obs_data_get_int(settings, "pcm_downsample_ratio"))
            obs_data_set_int(settings, "pcm_downsample_ratio", ratio);
        d->router.set_pcm_downsample_ratio(ratio);
    }
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
    maybe_autofill_rtmp_url(settings, false);
    {
        int ws_port = (int)obs_data_get_int(settings, "ws_port");
        if (ws_port < 1 || ws_port > 65535) {
            ws_port = WS_PORT;
            obs_data_set_int(settings, "ws_port", ws_port);
        }
        d->ws_port.store(ws_port, std::memory_order_relaxed);
    }
    const char* hip = obs_data_get_string(settings, "host_ip_manual");
    {
        std::lock_guard<std::mutex> lk(d->stream_id_mtx);
        d->host_ip = (hip && *hip) ? hip : d->auto_ip;
    }
    d->master_delay_ms = (float)obs_data_get_double(settings, "master_delay_ms");
    d->master_buf.set_delay_ms(d->enabled.load() ? (uint32_t)d->master_delay_ms : 0);
    d->sub_offset_ms = (float)obs_data_get_double(settings, "sub_offset_ms");
    for (int i = 0; i < MAX_SUB_CH; ++i) {
        char key[32]; snprintf(key, sizeof(key), "sub%d_delay_ms", i);
        d->sub[i].delay_ms = (float)obs_data_get_double(settings, key);
        char adjust_key[32]; snprintf(adjust_key, sizeof(adjust_key), "sub%d_adjust_ms", i);
        float adjust = (float)obs_data_get_double(settings, adjust_key);
        if (adjust < SUB_ADJUST_MIN_MS) {
            adjust = SUB_ADJUST_MIN_MS;
            obs_data_set_double(settings, adjust_key, adjust);
        } else if (adjust > SUB_ADJUST_MAX_MS) {
            adjust = SUB_ADJUST_MAX_MS;
            obs_data_set_double(settings, adjust_key, adjust);
        }
        d->sub[i].adjust_ms = adjust;
        char memo_key[32]; snprintf(memo_key, sizeof(memo_key), "sub%d_memo", i);
        const char* memo = obs_data_get_string(settings, memo_key);
        d->router.set_sub_memo(i, memo ? memo : "");
        apply_sub_delay_to_buffer(d, i);
    }
    d->rtmp_url_auto.store(obs_data_get_bool(settings, "rtmp_url_auto"), std::memory_order_relaxed);
    if (detail_changed) {
        // UI構造が変わるため、設定反映後にプロパティを再構築する。
        request_properties_refresh(d, "ds_update.detail_changed");
    }
    d->prev_stream_id_has_user_value = obs_data_has_user_value(settings, "stream_id");
}

// マスター遅延適用とチャンネル配信用の音声分岐を行う。
static obs_audio_data* ds_filter_audio(void* data, obs_audio_data* audio) {
    auto* d = static_cast<DelayStreamData*>(data);
    if (d->is_duplicate_instance) return audio;
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
// 選択されたチャンネルの往復遅延測定を開始する。
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
    request_properties_refresh(d, "cb_sub_measure");
    return false;
}
// 追加遅延スライダー変更を即時反映する。
static bool cb_sub_adjust_changed(void* priv, obs_properties_t*, obs_property_t*, obs_data_t* settings) {
    auto* ctx = static_cast<ChCtx*>(priv);
    if (!ctx || !ctx->d || !settings) return false;
    auto* d = ctx->d;
    int i = ctx->ch;
    if (i < 0 || i >= MAX_SUB_CH) return false;

    char key[32];
    snprintf(key, sizeof(key), "sub%d_adjust_ms", i);
    float adjust = (float)obs_data_get_double(settings, key);
    if (adjust < SUB_ADJUST_MIN_MS) {
        adjust = SUB_ADJUST_MIN_MS;
        obs_data_set_double(settings, key, adjust);
    } else if (adjust > SUB_ADJUST_MAX_MS) {
        adjust = SUB_ADJUST_MAX_MS;
        obs_data_set_double(settings, key, adjust);
    }

    float prev_adjust = d->sub[i].adjust_ms;
    bool changed = std::fabs(prev_adjust - adjust) > 0.0001f;
    d->sub[i].adjust_ms = adjust;
    apply_sub_delay_to_buffer(d, i);
    d->router.notify_apply_delay(
        i,
        calc_effective_sub_delay_value_ms(
            d, d->sub[i].delay_ms, d->sub[i].adjust_ms),
        "manual_adjust");
    if (changed) {
        request_properties_refresh(d, "cb_sub_adjust_changed");
    }
    return false;
}
// 指定チャンネルの遅延を設定値と実バッファへ反映する。
static void apply_sub_delay(DelayStreamData* d, int i, double ms) {
    if (!d || i < 0 || i >= MAX_SUB_CH) return;
    obs_data_t* s = obs_source_get_settings(d->context);
    char key[32]; snprintf(key, sizeof(key), "sub%d_delay_ms", i);
    obs_data_set_double(s, key, ms);
    obs_data_release(s);
    d->sub[i].delay_ms = (float)ms;
    apply_sub_delay_to_buffer(d, i);
    d->router.notify_apply_delay(
        i,
        d->sub[i].delay_ms,
        "auto_measure");
}
// 全チャンネルのURLと名前の一覧をMarkdown箇条書き形式でクリップボードへコピーする。
static bool cb_sub_copy_all(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    if (!d) return false;
    obs_data_t* s = obs_source_get_settings(d->context);
    std::string out;
    out.reserve(512);
    out += "演者は各自、以下のURLをChromeで開いて音声ストリームを再生してください。\r\n\r\n";
    int sub_count = d->sub_ch_count;
    for (int i = 0; i < sub_count; ++i) {
        std::string url = make_sub_url(d, i + 1);
        char memo_key[32]; snprintf(memo_key, sizeof(memo_key), "sub%d_memo", i);
        const char* memo = obs_data_get_string(s, memo_key);
        const char* url_show = url.empty() ? T_("NotConfigured") : url.c_str();
        out += "- Ch.";
        out += std::to_string(i + 1);
        out += " ";
        if (memo && *memo) out += memo;
        out += " ";
        out += url_show;
        out += "\r\n";
    }
    obs_data_release(s);
    if (!out.empty()) copy_to_clipboard(out);
    return false;
}
// チャンネルを1つ追加する。
static bool cb_sub_add(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    if (!d) return false;
    if (d->router_running.load()) return false;
    int cur = d->sub_ch_count;
    if (cur >= MAX_SUB_CH) return false;
    int next = clamp_sub_ch_count(cur + 1);
    int added_ch = next - 1;
    obs_data_t* s = obs_source_get_settings(d->context);

    // 追加対象の名前が未設定のときだけ自動命名する。
    char memo_key[32];
    snprintf(memo_key, sizeof(memo_key), "sub%d_memo", added_ch);
    const char* memo = obs_data_get_string(s, memo_key);
    if (!memo || !*memo) {
        int counter = (int)obs_data_get_int(s, "sub_memo_auto_counter");
        if (counter < 0) counter = 0;
        std::string auto_memo = make_default_sub_memo(counter);
        obs_data_set_string(s, memo_key, auto_memo.c_str());
        obs_data_set_int(s, "sub_memo_auto_counter", counter + 1);
        d->router.set_sub_memo(added_ch, auto_memo);
    } else {
        d->router.set_sub_memo(added_ch, memo);
    }

    obs_data_set_int(s, "sub_ch_count", next);
    obs_data_release(s);
    blog(LOG_INFO, "[obs-delay-stream] cb_sub_add sub_ch_count %d -> %d", cur, next);
    d->sub_ch_count = next;
    d->router.set_active_channels(next);
    d->flow.set_active_channels(next);
    d->flow.reset();
    request_properties_refresh(d, "cb_sub_add");
    return false;
}
// 指定チャンネルを削除し、後続チャンネルを前詰めする。
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
    for (int i = ch; i < MAX_SUB_CH - 1; ++i) {
        char key_from[32], key_to[32];
        snprintf(key_from, sizeof(key_from), "sub%d_delay_ms", i + 1);
        snprintf(key_to,   sizeof(key_to),   "sub%d_delay_ms", i);
        double v = obs_data_get_double(s, key_from);
        obs_data_set_double(s, key_to, v);

        char adjust_from[32], adjust_to[32];
        snprintf(adjust_from, sizeof(adjust_from), "sub%d_adjust_ms", i + 1);
        snprintf(adjust_to,   sizeof(adjust_to),   "sub%d_adjust_ms", i);
        double av = obs_data_get_double(s, adjust_from);
        obs_data_set_double(s, adjust_to, av);

        char memo_from[32], memo_to[32];
        snprintf(memo_from, sizeof(memo_from), "sub%d_memo", i + 1);
        snprintf(memo_to,   sizeof(memo_to),   "sub%d_memo", i);
        const char* m = obs_data_get_string(s, memo_from);
        obs_data_set_string(s, memo_to, m ? m : "");
        d->router.set_sub_memo(i, m ? m : "");
    }
    // clear last
    {
        char key_last[32]; snprintf(key_last, sizeof(key_last), "sub%d_delay_ms", MAX_SUB_CH - 1);
        obs_data_set_double(s, key_last, 0.0);
        char adjust_last[32]; snprintf(adjust_last, sizeof(adjust_last), "sub%d_adjust_ms", MAX_SUB_CH - 1);
        obs_data_set_double(s, adjust_last, 0.0);
    }
    obs_data_set_int(s, "sub_ch_count", next);
    obs_data_release(s);
    blog(LOG_INFO, "[obs-delay-stream] cb_sub_remove sub_ch_count %d -> %d (remove ch=%d)",
         cur, next, ch + 1);

    // shift runtime state down
    for (int i = ch; i < MAX_SUB_CH - 1; ++i) {
        d->sub[i].delay_ms = d->sub[i + 1].delay_ms;
        d->sub[i].adjust_ms = d->sub[i + 1].adjust_ms;
        apply_sub_delay_to_buffer(d, i);
        clear_measure_state(d->sub[i].measure);
    }
    d->sub[MAX_SUB_CH - 1].delay_ms = 0.0f;
    d->sub[MAX_SUB_CH - 1].adjust_ms = 0.0f;
    apply_sub_delay_to_buffer(d, MAX_SUB_CH - 1);
    clear_measure_state(d->sub[MAX_SUB_CH - 1].measure);

    d->sub_ch_count = next;
    d->router.set_active_channels(next);
    d->flow.set_active_channels(next);
    d->flow.reset();
    request_properties_refresh(d, "cb_sub_remove");
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
    request_properties_refresh(d, "cb_rtmp_apply");
    return false;
}
// RTMP URL自動取得ON/OFF切り替え時の処理。
static bool cb_rtmp_url_auto_changed(void* priv, obs_properties_t* props, obs_property_t*, obs_data_t* settings) {
    auto* d = static_cast<DelayStreamData*>(priv);
    if (!d || !settings) return false;
    bool auto_new = obs_data_get_bool(settings, "rtmp_url_auto");
    d->rtmp_url_auto.store(auto_new, std::memory_order_relaxed);
    if (auto_new) {
        maybe_autofill_rtmp_url(settings, true);
    }
    if (props) {
        if (auto* url_p = obs_properties_get(props, "rtmp_url")) {
            obs_property_set_enabled(url_p, !auto_new);
        }
    }
    return true;
}
// WebSocketサーバーを起動する。
static bool cb_ws_server_start(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    if (d->router_running.load()) return false;
    int ws_port = d->ws_port.load(std::memory_order_relaxed);
    if (d->router.start((uint16_t)ws_port)) {
        d->router_running.store(true);
        blog(LOG_INFO, "[obs-delay-stream] WebSocket server started on port %d", ws_port);
    } else {
        blog(LOG_ERROR, "[obs-delay-stream] WebSocket server FAILED to start on port %d", ws_port);
    }
    request_properties_refresh(d, "cb_ws_server_start");
    return false;
}

// WebSocketサーバーを停止する。
static bool cb_ws_server_stop(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    if (!d->router_running.load()) return false;
    d->router.stop();
    d->router_running.store(false);
    blog(LOG_INFO, "[obs-delay-stream] WebSocket server stopped");
    request_properties_refresh(d, "cb_ws_server_stop");
    return false;
}

// cloudflaredトンネルを起動する。
static bool cb_tunnel_start(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    obs_data_t* s = obs_source_get_settings(d->context);
    const char* exe = obs_data_get_string(s, "cloudflared_exe_path");
    obs_data_release(s);
    int ws_port = d->ws_port.load(std::memory_order_relaxed);
    d->tunnel.start(exe ? exe : "", ws_port);
    request_properties_refresh(d, "cb_tunnel_start");
    return false;
}
// トンネルを停止する。
static bool cb_tunnel_stop(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    d->tunnel.stop();
    request_properties_refresh(d, "cb_tunnel_stop");
    return false;
}
// Sync Flow Step1（チャンネル測定）を開始する。
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
    request_properties_refresh(d, "cb_flow_reset");
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
    for (int i = 0; i < MAX_SUB_CH; ++i)
        apply_sub_delay_to_buffer(d, i);
    request_properties_refresh(d, "cb_enabled_changed");
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

static void apply_codec_option_visibility(obs_properties_t* props, obs_data_t* settings) {
    if (!props || !settings) return;
    int codec = (int)obs_data_get_int(settings, "audio_codec");
    bool detail = obs_data_get_bool(settings, "detail_mode");
    bool is_opus = (codec == 0);
    bool show_opus_options = is_opus && detail;
    bool show_pcm_options = !is_opus;
    if (auto* p = obs_properties_get(props, "opus_bitrate_kbps")) {
        obs_property_set_visible(p, show_opus_options);
    }
    if (auto* p = obs_properties_get(props, "opus_sample_rate")) {
        obs_property_set_visible(p, show_opus_options);
    }
    if (auto* p = obs_properties_get(props, "pcm_input_sample_rate_info")) {
        obs_property_set_visible(p, show_pcm_options);
    }
    if (auto* p = obs_properties_get(props, "quantization_bits")) {
        obs_property_set_visible(p, show_pcm_options);
    }
    if (auto* p = obs_properties_get(props, "audio_mono")) {
        obs_property_set_visible(p, show_pcm_options);
    }
    if (auto* p = obs_properties_get(props, "pcm_downsample_ratio")) {
        obs_property_set_visible(p, show_pcm_options);
    }
}

static bool cb_audio_codec_changed(void* priv, obs_properties_t* props, obs_property_t*, obs_data_t* settings) {
    (void)priv;
    apply_codec_option_visibility(props, settings);
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
    if (auto* p = obs_properties_get(props, "ws_port")) {
        obs_property_set_visible(p, detail);
    }
    int sub_count = d->sub_ch_count;
    for (int i = 0; i < sub_count; ++i) {
        char uk[32], aj[32], mk[32];
        snprintf(uk, sizeof(uk), "sub%d_url", i);
        snprintf(aj, sizeof(aj), "sub%d_adjust_ms", i);
        snprintf(mk, sizeof(mk), "sub%d_meas", i);
        if (auto* p = obs_properties_get(props, uk)) {
            obs_property_set_visible(p, detail);
        }
        char ei[32];
        snprintf(ei, sizeof(ei), "sub%d_effective_info", i);
        if (auto* p = obs_properties_get(props, ei)) {
            obs_property_set_visible(p, detail);
        }
        if (auto* p = obs_properties_get(props, aj)) {
            obs_property_set_visible(p, detail);
        }
        if (auto* p = obs_properties_get(props, mk)) {
            obs_property_set_visible(p, detail);
        }
    }
}
// 詳細表示モード切り替え時に内部状態だけを更新するコールバック。
static bool cb_detail_mode_changed(void* priv, obs_properties_t* props, obs_property_t*, obs_data_t* settings) {
    auto* d = static_cast<DelayStreamData*>(priv);
    if (!props || !settings || !d) return false;
    bool detail = obs_data_get_bool(settings, "detail_mode");
    d->detail_mode.store(detail, std::memory_order_relaxed);
    // UI構造の切り替えは ds_update 側の再構築要求で行う。
    return false;
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

        obs_data_t* s = obs_source_get_settings(d->context);
        auto format_sub_name = [s](int ch) -> std::string {
            if (!s) return "Ch." + std::to_string(ch + 1);
            char memo_key[32];
            snprintf(memo_key, sizeof(memo_key), "sub%d_memo", ch);
            const char* memo = obs_data_get_string(s, memo_key);
            if (memo && *memo) return std::string(memo);
            return "Ch." + std::to_string(ch + 1);
        };

        std::string connected_names;
        std::string disconnected_names;
        int nc = 0;
        int nd = 0;
        for (int i = 0; i < sub_count; ++i) {
            std::string name = format_sub_name(i);
            if (d->router.client_count(i) > 0) {
                if (!connected_names.empty()) connected_names += " ";
                connected_names += name;
                ++nc;
            } else {
                if (!disconnected_names.empty()) disconnected_names += " ";
                disconnected_names += name;
                ++nd;
            }
        }
        if (s) obs_data_release(s);

        if (nc == 0) connected_names = T_("FlowNone");
        if (nd == 0) disconnected_names = T_("FlowNone");

        std::string status_text = std::string(T_("FlowConnected")) + connected_names +
                                  "\n" + T_("FlowDisconnected") + disconnected_names;
        obs_properties_add_text(props, "flow_connected", status_text.c_str(), OBS_TEXT_INFO);
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
        char buf[1536];
        snprintf(buf, sizeof(buf), "%s", T_("FlowStep1DoneApplied"));
        obs_data_t* s = obs_source_get_settings(d->context);
        for (int i = 0; i < sub_count; ++i) {
            if (!res.channels[i].connected) continue;
            char memo_key[32];
            snprintf(memo_key, sizeof(memo_key), "sub%d_memo", i);
            const char* memo = s ? obs_data_get_string(s, memo_key) : "";
            std::string name = (memo && *memo) ? memo : ("Ch." + std::to_string(i + 1));
            char line[192];
            if (res.channels[i].measured) {
                // 個別計測の自動適用後、現在の基準遅延値を表示する。
                snprintf(line, sizeof(line), "\n  Ch.%d %s : %.1f ms",
                         i + 1, name.c_str(), d->sub[i].delay_ms);
            } else {
                snprintf(line, sizeof(line), "\n  Ch.%d %s : %s",
                         i + 1, name.c_str(), T_("FlowChFailed"));
            }
            strncat(buf, line, sizeof(buf)-strlen(buf)-1);
        }
        if (s) obs_data_release(s);
        obs_properties_add_text(props, "flow_s1_result", buf, OBS_TEXT_INFO);
        // 失敗CHがあればリトライボタンを表示
        if (res.measured_count < res.connected_count)
            obs_properties_add_button2(props, "flow_retry_btn",
                T_("FlowRetryFailed"), cb_flow_retry_failed, d);
        obs_properties_add_button2(props, "flow_s3_btn",
            T_("FlowStep2Start"), cb_flow_start_step3, d);
        obs_properties_add_button2(props, "flow_cancel_s1d", T_("Cancel"), cb_flow_reset, d);
        break;
    }
    case FlowPhase::Step3_Measuring:
        obs_properties_add_text(props, "flow_s3_prog", T_("FlowStep2Measuring"), OBS_TEXT_INFO);
        obs_properties_add_button2(props, "flow_cancel_s3m", T_("Cancel"), cb_flow_reset, d);
        break;
    case FlowPhase::Step3_Done: {
        char buf[512];
        if (res.rtmp_valid) {
            snprintf(buf, sizeof(buf),
                T_("FlowStep2ResultFmt"),
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
    if (!d) return;
    has_sid = !d->get_stream_id().empty();
    detail_mode = d->detail_mode.load(std::memory_order_relaxed);
}

// プラグイン情報と詳細モードをまとめた先頭グループを追加する。
static void add_plugin_group(obs_properties_t* props, DelayStreamData* d) {
    if (!props || !d) return;
    obs_properties_t* grp = obs_properties_create();

    obs_property_t* about_p = obs_properties_add_text(grp, "about_info", "", OBS_TEXT_INFO);
    obs_property_set_long_description(about_p,
        "v" PLUGIN_VERSION " | (C) 2026 Mazzn1987, Chigiri Tsutsumi | GPL 2.0+<br>"
        "<a href=\"https://github.com/MZ1987Records/obs-delay-stream\">GitHub</a> | "
        "<a href=\"https://mz1987records.booth.pm/items/8134637\">Booth</a>");
    obs_property_text_set_info_word_wrap(about_p, false);

    if (d->is_duplicate_instance) {
        obs_properties_add_text(
            grp,
            "duplicate_instance_warning",
            "複数の obs-delay-stream フィルタを使用することはできません。",
            OBS_TEXT_INFO);
    } else {
        obs_property_t* detail_p =
            obs_properties_add_bool(grp, "detail_mode", T_("DetailMode"));
        obs_property_set_modified_callback2(detail_p, cb_detail_mode_changed, d);
    }

    obs_properties_add_group(props, "grp_plugin", T_("Plugin"), OBS_GROUP_NORMAL, grp);
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
    obs_property_t* port_p =
        obs_properties_add_int(grp, "ws_port", T_("WsPort"), 1, 65535, 1);
    if (d->router_running.load()) {
        obs_property_set_enabled(sid_p, false);
        obs_property_set_enabled(ip_p, false);
        obs_property_set_enabled(port_p, false);
    }
    obs_properties_add_group(props, "grp_stream", T_("GroupStreamId"), OBS_GROUP_NORMAL, grp);
}

// WebSocket 設定グループ
static void add_ws_group(obs_properties_t* props, DelayStreamData* d, bool has_sid) {
    if (!props || !d) return;
    bool ws_running = d->router_running.load();
    int ws_port = ws_running
        ? (int)d->router.port()
        : d->ws_port.load(std::memory_order_relaxed);

    char ws_title[96];
    if (ws_running)
        snprintf(ws_title, sizeof(ws_title), T_("WsRunningFmt"), ws_port);
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
    obs_property_set_modified_callback2(codec_p, cb_audio_codec_changed, d);
    obs_property_list_add_int(codec_p, "Opus", 0);
    obs_property_list_add_int(codec_p, T_("CodecPcm"), 1);

    obs_property_t* opus_bitrate_p = obs_properties_add_int(
        grp, "opus_bitrate_kbps", T_("OpusBitrateKbps"), 6, 510, 1);
    obs_property_t* opus_sample_rate_p = obs_properties_add_list(
        grp, "opus_sample_rate", T_("OpusSampleRate"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(opus_sample_rate_p, T_("OpusSampleRateAuto"), 0);
    obs_property_list_add_int(opus_sample_rate_p, "8000", 8000);
    obs_property_list_add_int(opus_sample_rate_p, "12000", 12000);
    obs_property_list_add_int(opus_sample_rate_p, "16000", 16000);
    obs_property_list_add_int(opus_sample_rate_p, "24000", 24000);
    obs_property_list_add_int(opus_sample_rate_p, "48000", 48000);
    uint32_t input_sr = d->sample_rate > 0 ? d->sample_rate : 48000;
    char pcm_sr_info[128];
    snprintf(pcm_sr_info, sizeof(pcm_sr_info), T_("PcmInputSampleRateFmt"), input_sr);
    obs_property_t* pcm_sr_info_p = obs_properties_add_text(
        grp, "pcm_input_sample_rate_info", pcm_sr_info, OBS_TEXT_INFO);
    obs_property_text_set_info_word_wrap(pcm_sr_info_p, false);
    obs_property_t* quant_bits_p = obs_properties_add_list(
        grp, "quantization_bits", T_("QuantizationBits"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(quant_bits_p, "8", 8);
    obs_property_list_add_int(quant_bits_p, "16", 16);
    obs_property_t* mono_mix_p =
        obs_properties_add_bool(grp, "audio_mono", T_("AudioMono"));
    obs_property_t* pcm_ds_ratio_p = obs_properties_add_list(
        grp, "pcm_downsample_ratio", T_("PcmDownsampleRatio"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(pcm_ds_ratio_p, T_("PcmDownsampleRatioNone"), 1);
    obs_property_list_add_int(pcm_ds_ratio_p, "1/2", 2);
    obs_property_list_add_int(pcm_ds_ratio_p, "1/4", 4);

    if (ws_running) {
        obs_property_set_enabled(codec_p, false);
        obs_property_set_enabled(opus_bitrate_p, false);
        obs_property_set_enabled(opus_sample_rate_p, false);
        obs_property_set_enabled(quant_bits_p, false);
        obs_property_set_enabled(mono_mix_p, false);
        obs_property_set_enabled(pcm_ds_ratio_p, false);
    }

    if (d->context) {
        obs_data_t* s = obs_source_get_settings(d->context);
        if (s) {
            apply_codec_option_visibility(grp, s);
            obs_data_release(s);
        }
    }

    if (ws_running) {
        obs_properties_add_button2(grp, "ws_server_stop_btn",
            T_("WsServerStop"), cb_ws_server_stop, d);
    } else {
        obs_property_t* start_p = obs_properties_add_button2(grp, "ws_server_start_btn",
            T_("WsServerStart"), cb_ws_server_start, d);
        obs_property_set_enabled(start_p, has_sid);
    }
    char ws_firewall_note[160];
    snprintf(ws_firewall_note, sizeof(ws_firewall_note),
        T_("WsFirewallNoteFmt"), ws_port);
    obs_property_t* fw_note_p = obs_properties_add_text(
        grp, "ws_firewall_note", ws_firewall_note, OBS_TEXT_INFO);
    obs_property_text_set_info_word_wrap(fw_note_p, false);

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
    obs_properties_add_text(grp, "cloudflared_exe_path", T_("CloudflaredExePath"), OBS_TEXT_DEFAULT);

    bool cloudflared_downloading =
        d->tunnel.cloudflared_downloading();
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

// URL配布グループを追加する。
static void add_url_share_group(obs_properties_t* props, DelayStreamData* d) {
    if (!props || !d) return;
    obs_properties_t* grp = obs_properties_create();
    TunnelState ts = d->tunnel.state();
    bool via_tunnel = !d->tunnel.url().empty();
    const char* suffix = via_tunnel
        ? T_("UrlShareTunnelSuffix")
        : T_("UrlShareDirectSuffix");
    char copy_label[192];
    snprintf(copy_label, sizeof(copy_label), "%s%s", T_("UrlShareCopyAll"), suffix);
    obs_property_t* copy_p =
        obs_properties_add_button2(grp, "url_share_copy_all", copy_label, cb_sub_copy_all, d);
    if (ts == TunnelState::Starting) {
        obs_property_set_enabled(copy_p, false);
    }
    obs_properties_add_group(props, "grp_url_share", T_("GroupUrlShare"), OBS_GROUP_NORMAL, grp);
}

// Sync Flowグループを追加する。
static void add_flow_group(obs_properties_t* props, DelayStreamData* d) {
    if (!props || !d) return;
    obs_properties_t* grp = obs_properties_create();
    build_flow_panel(grp, d);
    int active_channels = d->sub_ch_count;
    int connected_channels = 0;
    for (int i = 0; i < active_channels; ++i) {
        if (d->router.client_count(i) > 0) {
            ++connected_channels;
        }
    }
    char flow_title[192];
    snprintf(flow_title, sizeof(flow_title), T_("GroupSyncFlowWithConn"),
             T_("GroupSyncFlow"), connected_channels, active_channels);
    obs_properties_add_group(props, "grp_flow", flow_title, OBS_GROUP_NORMAL, grp);
}

// Master遅延/RTMP測定グループを追加する。
static void add_master_group(obs_properties_t* props, DelayStreamData* d) {
    if (!props || !d) return;
    obs_properties_t* grp = obs_properties_create();
    obs_property_t* mp = obs_properties_add_float_slider(
        grp, "master_delay_ms", T_("MasterDelay"), 0.0, MAX_DELAY_MS, 1.0);
    obs_property_float_set_suffix(mp, " ms");
    bool auto_mode = true;
    {
        obs_data_t* s = obs_source_get_settings(d->context);
        if (s) {
            auto_mode = obs_data_get_bool(s, "rtmp_url_auto");
            obs_data_release(s);
        }
    }
    obs_property_t* auto_p = obs_properties_add_bool(grp, "rtmp_url_auto", T_("RtmpUrlAuto"));
    obs_property_set_modified_callback2(auto_p, cb_rtmp_url_auto_changed, d);
    obs_property_t* url_p =
        obs_properties_add_text(grp, "rtmp_url", T_("RtmpUrl"), OBS_TEXT_DEFAULT);
    obs_property_set_enabled(url_p, !auto_mode);
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

// チャンネル共通オフセット設定グループを追加する。
static void add_sub_offset_group(obs_properties_t* props, DelayStreamData* d) {
    if (!props || !d) return;
    obs_properties_t* grp = obs_properties_create();
    {
        char offset_info[256];
        snprintf(offset_info, sizeof(offset_info),
            T_("GlobalOffsetInfoFmt"), d->sub_offset_ms);
        obs_properties_add_text(grp, "sub_offset_info", offset_info, OBS_TEXT_INFO);
    }
    obs_property_t* op = obs_properties_add_float_slider(
        grp, "sub_offset_ms",
        T_("GlobalOffsetLabel"), -2000.0, 5000.0, 10.0);
    obs_property_float_set_suffix(op, " ms");
    obs_properties_add_group(props, "grp_offset",
        T_("GroupGlobalOffset"), OBS_GROUP_NORMAL, grp);
}

// チャンネルの測定状態に応じてボタン/結果表示を切り替える。
static void add_sub_channel_measure_controls(obs_properties_t* ch_grp,
                                             DelayStreamData* d,
                                             int i,
                                             size_t nc,
                                             const char* mk,
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

// チャンネル 1件分のUIを構築する（詳細モード）。
static void add_sub_channel_item_detail(obs_properties_t* grp, DelayStreamData* d, int i, int sub_count) {
    if (!grp || !d) return;
    d->btn_ctx[i] = { d, i };

    char uk[32], mk[32], rk[32], nk[32], dk_rm[32], ajk[32], eik[32];
    char ul[32], us[512], gk[32], gt[32];
    snprintf(uk, sizeof(uk), "sub%d_url", i);
    snprintf(mk, sizeof(mk), "sub%d_meas", i);
    snprintf(rk, sizeof(rk), "sub%d_result", i);
    snprintf(nk, sizeof(nk), "sub%d_memo", i);
    snprintf(dk_rm, sizeof(dk_rm), "sub%d_remove", i);
    snprintf(ajk, sizeof(ajk), "sub%d_adjust_ms", i);
    snprintf(eik, sizeof(eik), "sub%d_effective_info", i);
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
    obs_property_t* sp = obs_properties_add_int_slider(
        ch_grp, ajk, T_("SubAdjust"),
        (int)SUB_ADJUST_MIN_MS, (int)SUB_ADJUST_MAX_MS, 1);
    obs_property_int_set_suffix(sp, " ms");
    obs_property_set_modified_callback2(sp, cb_sub_adjust_changed, &d->btn_ctx[i]);
    char ei[192];
    float effective = calc_effective_sub_delay_value_ms(
        d, d->sub[i].delay_ms, d->sub[i].adjust_ms);
    snprintf(ei, sizeof(ei), T_("SubDelayEffectiveInfoFmt"),
             effective, d->sub[i].delay_ms, d->sub[i].adjust_ms, d->sub_offset_ms);
    obs_properties_add_text(ch_grp, eik, ei, OBS_TEXT_INFO);

    add_sub_channel_measure_controls(ch_grp, d, i, nc, mk, rk);

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

// チャンネル 1件分のUIを構築する（簡易モード）。
static void add_sub_channel_item_simple(obs_properties_t* grp, DelayStreamData* d, int i, int sub_count) {
    if (!grp || !d) return;
    d->btn_ctx[i] = { d, i };

    char nk[32], dk_rm[32], lt[32];
    snprintf(nk, sizeof(nk), "sub%d_memo", i);
    snprintf(dk_rm, sizeof(dk_rm), "sub%d_remove", i);
    snprintf(lt, sizeof(lt), "Ch.%d", i + 1);

    // 簡易モードでは、メモ入力欄のラベル位置にチャンネル番号を表示する。
    obs_property_t* memo_p = obs_properties_add_text(grp, nk, lt, OBS_TEXT_DEFAULT);
    if (d->router_running.load()) {
        obs_property_set_enabled(memo_p, false);
    }

    char rm_label[32];
    snprintf(rm_label, sizeof(rm_label), T_("SubRemoveFmt"), i + 1);
    obs_property_t* rm = obs_properties_add_button2(grp, dk_rm, rm_label,
        cb_sub_remove, &d->btn_ctx[i]);
    obs_property_set_long_description(rm, T_("SubRemoveDesc"));
    if (d->router_running.load() || sub_count <= 1) {
        obs_property_set_enabled(rm, false);
    }
}

// チャンネル 一覧グループを構築する（詳細/簡易モードで項目を切り替える）。
static void add_sub_channels_group(obs_properties_t* props, DelayStreamData* d, bool detail_mode) {
    if (!props || !d) return;
    obs_properties_t* grp = obs_properties_create();
    int sub_count = d->sub_ch_count;
    for (int i = 0; i < sub_count; ++i) {
        if (detail_mode) {
            add_sub_channel_item_detail(grp, d, i, sub_count);
        } else {
            add_sub_channel_item_simple(grp, d, i, sub_count);
        }
    }
    obs_property_t* spc_bottom = obs_properties_add_text(grp, "sub_add_spacer", "", OBS_TEXT_INFO);
    obs_property_set_long_description(spc_bottom, " ");
    obs_property_text_set_info_word_wrap(spc_bottom, false);
    char add_label[64];
    if (d->sub_ch_count >= MAX_SUB_CH) {
        snprintf(add_label, sizeof(add_label), "%s", T_("SubAddLimitReached"));
    } else {
        snprintf(add_label, sizeof(add_label), T_("SubAddFmt"), d->sub_ch_count + 1);
    }
    obs_property_t* add_p =
        obs_properties_add_button2(grp, "sub_add_btn", add_label, cb_sub_add, d);
    if (d->router_running.load() || d->sub_ch_count >= MAX_SUB_CH) {
        obs_property_set_enabled(add_p, false);
    }
    obs_properties_add_group(props, "grp_sub", T_("GroupSubChannels"), OBS_GROUP_NORMAL, grp);
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

    // 自動取得ON時は、プロパティ表示のたびに最新の配信設定URLへ更新する。
    maybe_autofill_rtmp_url_from_source(d->context, true);

    maybe_fill_cloudflared_path_from_auto(d);

    bool has_sid = false;
    bool detail_mode = false;
    read_properties_view_state(d, has_sid, detail_mode);

    // UIブロックを順に組み立てる
    add_plugin_group(props, d);
    if (d->is_duplicate_instance) {
        d->in_get_props.store(false);
        return props;
    }
    add_sub_channels_group(props, d, detail_mode);
    add_stream_group(props, d);
    add_ws_group(props, d, has_sid);
    add_tunnel_group(props, d);
    add_url_share_group(props, d);
    add_flow_group(props, d);
    add_master_group(props, d);
    add_sub_offset_group(props, d);

    apply_detail_mode_visibility(props, d, detail_mode);

    d->in_get_props.store(false);
    return props;
}

// 各設定項目のデフォルト値を定義する。
static void ds_get_defaults(obs_data_t* settings) {
    obs_data_set_default_bool  (settings, "delay_disable",         false);
    obs_data_set_default_bool  (settings, "detail_mode",           false);
    obs_data_set_default_bool  (settings, "ws_send_paused",        false);
    obs_data_set_default_int   (settings, "sub_ch_count",          1);
    // Ch.1 既定名を A にするため、次の自動払い出しは B から開始する。
    obs_data_set_default_int   (settings, "sub_memo_auto_counter", 1);
    obs_data_set_default_int   (settings, "audio_codec",           0);
    obs_data_set_default_int   (settings, "opus_bitrate_kbps",     96);
    obs_data_set_default_int   (settings, "opus_sample_rate",      0);
    obs_data_set_default_int   (settings, "quantization_bits",     8);
    obs_data_set_default_bool  (settings, "audio_mono",            true);
    obs_data_set_default_int   (settings, "pcm_downsample_ratio",  1);
    obs_data_set_default_int   (settings, "ws_port",               WS_PORT);
    obs_data_set_default_string(settings, "stream_id",             "");
    obs_data_set_default_string(settings, "host_ip_manual",        "");
    obs_data_set_default_double(settings, "master_delay_ms",       0.0);
    obs_data_set_default_double(settings, "sub_offset_ms",         0.0);
    obs_data_set_default_bool  (settings, "rtmp_url_auto",         true);
    obs_data_set_default_string(settings, "rtmp_url",              "");
    obs_data_set_default_string(settings, "cloudflared_exe_path",  "");
    for (int i = 0; i < MAX_SUB_CH; ++i) {
        char key[32]; snprintf(key, sizeof(key), "sub%d_delay_ms", i);
        obs_data_set_default_double(settings, key, 0.0);
        char adjust_key[32]; snprintf(adjust_key, sizeof(adjust_key), "sub%d_adjust_ms", i);
        obs_data_set_default_double(settings, adjust_key, 0.0);
        char memo_key[32]; snprintf(memo_key, sizeof(memo_key), "sub%d_memo", i);
        if (i == 0) {
            std::string default_memo = make_default_sub_memo(0);
            obs_data_set_default_string(settings, memo_key, default_memo.c_str());
        } else {
            obs_data_set_default_string(settings, memo_key, "");
        }
    }
}
