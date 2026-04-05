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
#include "rtmp-prober.hpp"
#include "stream-router.hpp"
#include "sync-flow.hpp"
#include "tunnel-manager.hpp"

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

enum class UpdateCheckStatus : int {
    Unknown = 0,
    Checking,
    UpToDate,
    UpdateAvailable,
    Error,
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
    std::atomic<int> update_check_status{(int)UpdateCheckStatus::Unknown};
    std::atomic<bool> update_check_inflight{false};
    std::mutex update_check_mtx;
    std::string latest_release_version;
    std::string latest_release_url;
    std::string update_check_error;
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
};
