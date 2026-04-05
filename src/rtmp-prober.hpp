#pragma once

/*
 * rtmp-prober.hpp
 *
 * RTMPサーバーへの「計測専用」TCP接続を張り、
 * ハンドシェイク(C0+C1 → S0+S1+S2)のRTTから
 * ネットワーク片道レイテンシを推定する。
 *
 * 実際の配信接続には一切干渉しない。
 */

#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>

// ============================================================
// RtmpProbeResult
// ============================================================
struct RtmpProbeResult {
    bool   valid        = false;
    double avg_rtt_ms   = 0.0;   // 平均RTT (ms)
    double avg_latency_ms = 0.0;  // 推定片道レイテンシ = RTT/2
    double min_rtt_ms   = 0.0;
    double max_rtt_ms   = 0.0;
    double jitter_ms    = 0.0;   // RTT標準偏差
    int    samples      = 0;
    int    failed       = 0;     // 失敗した試行数
    std::string error_msg;
};

// ============================================================
// RtmpProber
// ============================================================
class RtmpProber {
public:
    // 計測完了コールバック（ワーカースレッドから呼ばれる）
    std::function<void(RtmpProbeResult)> on_result;

    RtmpProber();
    ~RtmpProber();

    // url_or_host: "rtmp://example.com/live/key" または "example.com:1935"
    bool start(const std::string& rtmp_url,
               int num_probes   = 10,
               int interval_ms  = 300);
    void cancel();

    bool            is_running()   const;
    RtmpProbeResult last_result()  const;
    std::string     host()         const;
    int             port()         const;

private:
    static bool parse_url(const std::string& url, std::string& host, int& port);
    double      probe_once();
    void        probe_loop();

    std::string       host_;
    int               port_        = 1935;
    int               num_probes_  = 10;
    int               interval_ms_ = 300;
    std::atomic<bool> running_{false};
    std::thread       worker_;
    mutable std::mutex mtx_;
    RtmpProbeResult   last_result_;
};
