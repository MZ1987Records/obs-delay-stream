#pragma once

#include <cstdint>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <obs-module.h>

#include "constants.hpp"
#include "delay-filter.hpp"
#include "plugin-main-props-refresh.hpp"
#include "rtmp-prober.hpp"
#include "stream-router.hpp"
#include "sync-flow.hpp"
#include "tunnel-manager.hpp"

// スレッドセーフな計測状態管理クラス。
// mutex を外部から直接触らずに済む操作インターフェースを提供する。
class MeasureState {
public:
    // 計測開始 (measuring=true、result/error をクリア)
    void start() {
        std::lock_guard<std::mutex> lk(mtx_);
        measuring_  = true;
        result_     = LatencyResult{};
        last_error_.clear();
    }
    // 計測完了 (measuring=false、結果とエラー文字列を設定)
    void set_result(const LatencyResult& r, const std::string& error = "") {
        std::lock_guard<std::mutex> lk(mtx_);
        result_     = r;
        measuring_  = false;
        applied_    = r.valid;
        last_error_ = error;
    }
    // 全状態リセット
    void reset() {
        std::lock_guard<std::mutex> lk(mtx_);
        result_     = LatencyResult{};
        measuring_  = false;
        applied_    = false;
        last_error_.clear();
    }
    bool          is_measuring() const { std::lock_guard<std::mutex> lk(mtx_); return measuring_; }
    LatencyResult result()       const { std::lock_guard<std::mutex> lk(mtx_); return result_; }
    bool          is_applied()   const { std::lock_guard<std::mutex> lk(mtx_); return applied_; }
    std::string   last_error()   const { std::lock_guard<std::mutex> lk(mtx_); return last_error_; }

private:
    mutable std::mutex mtx_;
    LatencyResult      result_;
    bool               measuring_  = false;
    bool               applied_    = false;
    std::string        last_error_;
};

// RtmpProber と計測結果をまとめたクラス。
// prober の on_result コールバックは外部から直接設定すること。
class RtmpMeasureState {
public:
    RtmpProber prober; // on_result は外部 (plugin-main.cpp) で設定

    // 計測結果をスレッドセーフに記録する (prober.on_result コールバックから呼ぶ)
    void apply_result(const RtmpProbeResult& r) {
        std::lock_guard<std::mutex> lk(mtx_);
        result_  = r;
        applied_ = false;
    }
    RtmpProbeResult result() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return result_;
    }
    bool is_applied() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return applied_;
    }
    void set_applied(bool v) {
        std::lock_guard<std::mutex> lk(mtx_);
        applied_ = v;
    }
    std::string cached_url() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return cached_url_;
    }
    void set_cached_url(const std::string& url) {
        std::lock_guard<std::mutex> lk(mtx_);
        cached_url_ = url;
    }

private:
    mutable std::mutex mtx_;
    RtmpProbeResult    result_;
    bool               applied_    = false;
    std::string        cached_url_;
};

struct DelayStreamData;
struct ChCtx { DelayStreamData* d; int ch; };

enum class UpdateCheckStatus {
    Unknown = 0,
    Checking,
    UpToDate,
    UpdateAvailable,
    Error,
};

// 更新確認状態をまとめたクラス。
// status / inflight はアトミック操作が必要なため public。
// 文字列フィールドはミューテックスで保護したアクセサ経由で読み書きする。
class UpdateCheckState {
public:
    std::atomic<UpdateCheckStatus> status{UpdateCheckStatus::Unknown};
    std::atomic<bool>              inflight{false};

    // 文字列フィールドをまとめて書き込む (ワーカースレッドから呼ぶ)
    void set_strings(const std::string& version, const std::string& url,
                     const std::string& error) {
        std::lock_guard<std::mutex> lk(mtx_);
        latest_version_ = version;
        latest_url_     = url;
        error_          = error;
    }
    std::string latest_version() const { std::lock_guard<std::mutex> lk(mtx_); return latest_version_; }
    std::string latest_url()     const { std::lock_guard<std::mutex> lk(mtx_); return latest_url_; }
    std::string error()          const { std::lock_guard<std::mutex> lk(mtx_); return error_; }

private:
    mutable std::mutex mtx_;
    std::string        latest_version_;
    std::string        latest_url_;
    std::string        error_;
};

struct DelayStreamData {
    obs_source_t* context      = nullptr;
    bool is_duplicate_instance = false;
    bool owns_singleton_slot   = false;
    uint64_t singleton_generation = 0;
    std::atomic<bool> destroying{false};
    std::atomic<bool> enabled{true};
    std::atomic<bool> ws_send_enabled{true};
    std::shared_ptr<std::atomic<bool>> life_token =
        std::make_shared<std::atomic<bool>>(true);
    mutable std::mutex stream_id_mtx;
    std::string   stream_id;
    std::string   host_ip;
    std::string   auto_ip;
    std::atomic<int> ws_port{WS_PORT};
    std::atomic<int> ping_count_setting{DEFAULT_PING_COUNT};
    int           playback_buffer_ms = PLAYBACK_BUFFER_DEFAULT_MS;
    float         master_delay_ms = 0.0f;
    float         sub_offset_ms   = 0.0f;
    int           sub_ch_count    = 1;
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
    std::atomic<bool> create_done{false};
    std::atomic<int> get_props_depth{0};
    std::atomic<int64_t> last_rendered_audio_sync_offset_ns{INT64_MIN};
    UpdateCheckState  update_check;
    std::atomic<bool> sid_autofill_guard{false};
    std::atomic<bool> rtmp_url_auto{true};
    bool prev_stream_id_has_user_value = false;
    std::vector<float> work_buf;

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
    // manual_override が空なら auto_ip にフォールバックする
    void set_host_ip(const char* manual_override) {
        std::lock_guard<std::mutex> lk(stream_id_mtx);
        host_ip = (manual_override && *manual_override) ? manual_override : auto_ip;
    }
    // プロパティUI の再描画を依頼する。
    // create_done / destroying / get_props_depth を自動参照するため呼び出し側で展開不要。
    void request_props_refresh(const char* reason = nullptr) const {
        plugin_main_props_refresh::props_refresh_request(
            context,
            create_done.load(std::memory_order_acquire),
            destroying.load(std::memory_order_acquire),
            get_props_depth.load(std::memory_order_acquire),
            reason);
    }
};
