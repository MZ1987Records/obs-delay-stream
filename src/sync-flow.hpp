#pragma once
/*
 * sync-flow.hpp
 *
 * 3ステップ同期フローを管理するクラス
 *
 * Step1: 接続中の全CHを並列計測（10回ping）
 * Step2: 最大片道遅延を基準に各CHの追加遅延を算出・確認表示
 * Step3: RTMP遅延計測 → マスター遅延 = パフォーマー基準 + RTMP片道遅延 を算出・確認表示
 *
 * 「遅い側に合わせる」設計:
 *   各サブch遅延 = max_one_way - ch_one_way  (差分を追加)
 *   マスター遅延 = max_one_way + rtmp_one_way (パフォーマー基準 + 配信遅延)
 */

#include "websocket-server.hpp"
#include "rtmp-prober.hpp"

#include <array>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <chrono>
#include <cstdio>
#include <algorithm>

#include "constants.hpp"

static constexpr int FLOW_PINGS    = 10;
static constexpr int FLOW_PING_INT = 150; // ms

// ============================================================
// フローの状態
// ============================================================
enum class FlowPhase {
    Idle,           // 待機中
    Step1_Measuring,// ステップ1: 全CH並列計測中
    Step1_Done,     // ステップ1完了 → 確認待ち
    Step2_Applied,  // ステップ2: サブch遅延反映済み → 確認待ち
    Step3_Measuring,// ステップ3: RTMP計測中
    Step3_Done,     // ステップ3完了 → 確認待ち
    Complete,       // 全完了
};

// ============================================================
// CH別計測結果サマリ
// ============================================================
struct ChSummary {
    int    ch;            // 0-indexed
    bool   connected;     // 接続中か
    bool   measured;      // 計測成功か
    double one_way_ms;    // 片道遅延推定値
    double proposed_delay;// 提案する遅延設定値
};

// ============================================================
// フロー全体の結果
// ============================================================
struct FlowResult {
    // Step1
    std::array<ChSummary, MAX_SUB_CH> channels{};
    int    connected_count  = 0;
    int    measured_count   = 0;
    double max_one_way_ms   = 0.0; // 基準(最大片道遅延)

    // Step3
    double rtmp_one_way_ms  = 0.0;
    double master_delay_ms  = 0.0; // = max_one_way + rtmp_one_way
    bool   rtmp_valid       = false;
    std::string rtmp_error;
};

// ============================================================
// SyncFlow
// ============================================================
class SyncFlow {
public:
    // GUI再描画要求コールバック
    std::function<void()>                          on_update;
    // 各CH計測完了通知
    std::function<void(int ch, LatencyResult)>     on_ch_measured;
    // サブch遅延一括書き込み要求
    std::function<void(const FlowResult&)>         on_apply_sub;
    // マスター遅延書き込み要求
    std::function<void(double master_ms)>          on_apply_master;

    SyncFlow() { reset(); }
    void set_active_channels(int n) {
        if (n < 1) n = 1;
        if (n > MAX_SUB_CH) n = MAX_SUB_CH;
        std::lock_guard<std::mutex> lk(mtx_);
        active_ch_ = n;
    }

    // ----- 状態アクセス -----
    FlowPhase   phase()  const { std::lock_guard<std::mutex> lk(mtx_); return phase_; }
    FlowResult  result() const { std::lock_guard<std::mutex> lk(mtx_); return result_; }
    bool        busy()   const {
        auto p = phase();
        return p == FlowPhase::Step1_Measuring || p == FlowPhase::Step3_Measuring;
    }

    // ----- ステップ1開始 -----
    // router: StreamRouter, rtmp_url: RTMP接続先URL
    bool start_step1(StreamRouter& router, const std::string& stream_id) {
        int active = MAX_SUB_CH;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (phase_ != FlowPhase::Idle) return false;
            active = active_ch_;
        }
        reset();
        {
            std::lock_guard<std::mutex> lk(mtx_);
            phase_ = FlowPhase::Step1_Measuring;
            // 接続中CHを確認
            for (int i = 0; i < active; ++i) {
                result_.channels[i].ch        = i;
                result_.channels[i].connected = (router.client_count(i) > 0);
                result_.channels[i].measured  = false;
                if (result_.channels[i].connected) ++result_.connected_count;
            }
        }

        if (result_.connected_count == 0) {
            std::lock_guard<std::mutex> lk(mtx_);
            phase_ = FlowPhase::Idle;
            return false;
        }

        // 各CHのコールバックを登録して並列計測開始
        pending_count_ = result_.connected_count;
        for (int i = 0; i < active; ++i) {
            if (!result_.channels[i].connected) continue;
            router.set_on_latency_result(i,
                [this, i](const std::string&, int, LatencyResult r) {
                    on_ch_result(i, r);
                });
            router.start_measurement(i, FLOW_PINGS, FLOW_PING_INT);
        }
        if (on_update) on_update();
        return true;
    }

    // ----- ステップ1: 失敗CHのみ再計測 -----
    bool retry_failed_step1(StreamRouter& router) {
        int retry_count = 0;
        int active = MAX_SUB_CH;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (phase_ != FlowPhase::Step1_Done) return false;
            active = active_ch_;

            // 接続中だが計測失敗のCHを特定
            for (int i = 0; i < active; ++i) {
                auto& ch = result_.channels[i];
                // 再接続している可能性があるので接続状態を再確認
                ch.connected = (router.client_count(i) > 0);
                if (ch.connected && !ch.measured) ++retry_count;
            }
            if (retry_count == 0) return false;

            phase_ = FlowPhase::Step1_Measuring;
        }

        // 失敗CHのみ再計測
        pending_count_ = retry_count;
        for (int i = 0; i < active; ++i) {
            auto& ch = result_.channels[i];
            if (!ch.connected || ch.measured) continue;
            router.set_on_latency_result(i,
                [this, i](const std::string&, int, LatencyResult r) {
                    on_ch_result(i, r);
                });
            router.start_measurement(i, FLOW_PINGS, FLOW_PING_INT);
        }
        if (on_update) on_update();
        return true;
    }

    // ----- ステップ2: サブch遅延一括反映 -----
    bool apply_step2() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (phase_ != FlowPhase::Step1_Done) return false;
        }
        if (on_apply_sub) on_apply_sub(result_);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            phase_ = FlowPhase::Step2_Applied;
        }
        if (on_update) on_update();
        return true;
    }

    // ----- ステップ3開始: RTMP計測 -----
    bool start_step3(const std::string& rtmp_url) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (phase_ != FlowPhase::Step2_Applied) return false;
            if (rtmp_url.empty()) return false;
            phase_ = FlowPhase::Step3_Measuring;
        }
        if (on_update) on_update();

        prober_.on_result = [this](RtmpProbeResult r) {
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (r.valid) {
                    result_.rtmp_valid      = true;
                    result_.rtmp_one_way_ms = r.avg_one_way;
                    // マスター遅延 = パフォーマー基準 + RTMP片道遅延
                    result_.master_delay_ms = result_.max_one_way_ms + r.avg_one_way;
                } else {
                    result_.rtmp_valid = false;
                    result_.rtmp_error = r.error_msg;
                }
                phase_ = FlowPhase::Step3_Done;
            }
            // コールバックはロック外で呼び出し（デッドロック防止）
            if (on_update) on_update();
        };
        prober_.start(rtmp_url, 10, 300);
        return true;
    }

    // ----- ステップ3: マスター遅延反映 -----
    bool apply_step3() {
        double ms;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (phase_ != FlowPhase::Step3_Done) return false;
            if (!result_.rtmp_valid) return false;
            ms = result_.master_delay_ms;
            phase_ = FlowPhase::Complete;
        }
        if (on_apply_master) on_apply_master(ms);
        if (on_update) on_update();
        return true;
    }

    // ----- リセット -----
    void reset() {
        prober_.cancel();
        std::lock_guard<std::mutex> lk(mtx_);
        phase_         = FlowPhase::Idle;
        pending_count_ = 0;
        result_        = FlowResult{};
    }

private:
    void on_ch_result(int i, LatencyResult r) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (i < 0 || i >= active_ch_) return;
            auto& ch = result_.channels[i];
            ch.measured  = r.valid;
            ch.one_way_ms = r.valid ? r.avg_one_way : 0.0;
            if (r.valid) ++result_.measured_count;

            if (--pending_count_ == 0) {
                // 全CH計測完了 → 提案値を算出
                compute_proposals();
                phase_ = FlowPhase::Step1_Done;
            }
        }
        if (on_ch_measured) on_ch_measured(i, r);
        if (on_update) on_update();
    }

    // 最大片道遅延を基準に各CHの提案遅延を計算
    void compute_proposals() {
        // 計測成功CHの最大片道遅延を求める
        double max_ow = 0.0;
        for (int i = 0; i < active_ch_; ++i) {
            auto& ch = result_.channels[i];
            if (ch.measured && ch.one_way_ms > max_ow)
                max_ow = ch.one_way_ms;
        }
        result_.max_one_way_ms = max_ow;

        // 各CHの追加遅延 = max - ch_one_way
        for (int i = 0; i < active_ch_; ++i) {
            auto& ch = result_.channels[i];
            if (ch.measured)
                ch.proposed_delay = max_ow - ch.one_way_ms;
            else
                ch.proposed_delay = 0.0; // 未計測CHは変更しない
        }
    }

    mutable std::mutex mtx_;
    FlowPhase          phase_         = FlowPhase::Idle;
    int                active_ch_     = MAX_SUB_CH;
    std::atomic<int>   pending_count_{0};
    FlowResult         result_;
    RtmpProber         prober_;
};
