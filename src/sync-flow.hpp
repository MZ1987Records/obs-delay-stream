#pragma once

/*
 * sync-flow.hpp
 *
 * 3ステップ同期フローを管理するクラス
 *
 * Step1: 接続中の全CHを並列計測（PING_COUNT回ping）
 * Step2: （廃止）
 * Step3: RTMP遅延計測 → マスター遅延 = パフォーマー基準 + RTMP片道遅延 を算出・確認表示
 *
 * マスター遅延の設計:
 *   マスター遅延 = max_one_way + rtmp_one_way (パフォーマー基準 + 配信遅延)
 */

#include <array>
#include <string>
#include <mutex>
#include <atomic>
#include <functional>

#include "constants.hpp"
#include "stream-router-types.hpp"  // LatencyResult
#include "rtmp-prober.hpp"          // RtmpProber, RtmpProbeResult

// StreamRouter は .cpp 側で include するため前方宣言のみ
class StreamRouter;

// ============================================================
// フローの状態
// ============================================================
enum class FlowPhase {
    Idle,            // 待機中
    Step1_Measuring, // ステップ1: 全CH並列計測中
    Step1_Done,      // ステップ1完了 → 確認待ち
    Step3_Measuring, // ステップ3: RTMP計測中
    Step3_Done,      // ステップ3完了 → 確認待ち
    Complete,        // 全完了
};

// ============================================================
// CH別計測結果サマリ
// ============================================================
struct ChSummary {
    int    ch;             // 0-indexed
    bool   connected;      // 接続中か
    bool   measured;       // 計測成功か
    double one_way_ms;     // 片道遅延推定値
    double proposed_delay; // 提案する遅延設定値
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
    std::function<void()>                      on_update;
    // 各CH計測完了通知
    std::function<void(int ch, LatencyResult)> on_ch_measured;
    // マスター遅延書き込み要求
    std::function<void(double master_ms)>      on_apply_master;

    SyncFlow();

    void set_active_channels(int n);

    FlowPhase  phase()  const;
    FlowResult result() const;
    bool       busy()   const;

    bool start_step1(StreamRouter& router, const std::string& stream_id);
    bool retry_failed_step1(StreamRouter& router);
    bool start_step3(const std::string& rtmp_url);
    bool apply_step3();
    void reset();

private:
    void on_ch_result(int i, LatencyResult r);
    void compute_proposals();

    mutable std::mutex mtx_;
    FlowPhase          phase_         = FlowPhase::Idle;
    int                active_ch_     = MAX_SUB_CH;
    std::atomic<int>   pending_count_{0};
    FlowResult         result_;
    RtmpProber         prober_;
};
