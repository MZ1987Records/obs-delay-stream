#pragma once

/*
 * sync-flow.hpp
 *
 * 3ステップ同期フローを管理するクラス
 *
 * Step1: 接続中の全CHを並列計測（デフォルト DEFAULT_PING_COUNT 回 ping、UI で変更可）
 * Step2: （廃止）
 * Step3: RTMPレイテンシ計測 → マスター遅延 = チャンネル最大レイテンシ + RTMP片道レイテンシ を算出・確認表示
 *
 * マスター遅延の設計:
 *   マスター遅延 = max_latency + rtmp_latency (チャンネル最大レイテンシ + 配信レイテンシ)
 */

#include "core/constants.hpp"
#include "network/rtmp-prober.hpp"         // RtmpProber, RtmpProbeResult
#include "network/stream-router-types.hpp" // LatencyResult

#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>

// StreamRouter は .cpp 側で include するため前方宣言のみ
namespace ods::network {
class StreamRouter;
}

namespace ods::sync {

using ods::core::MAX_SUB_CH;
using ods::core::DEFAULT_PING_COUNT;
using ods::network::LatencyResult;
using ods::network::RtmpProber;
using ods::network::RtmpProbeResult;

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
	int    ch;                 // 0-indexed
	bool   connected;          // 接続中か
	bool   measured;           // 計測成功か
	double one_way_latency_ms; // 片道レイテンシ推定値
	double proposed_delay;     // 提案する遅延設定値
};

// ============================================================
// フロー全体の結果
// ============================================================
struct FlowResult {
	// Step1
	std::array<ChSummary, MAX_SUB_CH> channels{};
	int                               connected_count  = 0;
	int                               completed_count  = 0; // 成功/失敗を問わず計測処理が完了した件数
	int                               measured_count   = 0;
	int                               ping_sent_count  = 0;   // 送信済みpingの合計数
	int                               ping_total_count = 0;   // 予定ping総数（connected_count × ping_count）
	double                            max_latency_ms   = 0.0; // 基準(最大片道レイテンシ)

	// Step3
	double      rtmp_latency_ms = 0.0;
	double      master_delay_ms = 0.0; // = max_latency + rtmp_latency
	bool        rtmp_valid      = false;
	std::string rtmp_error;
};

// ============================================================
// SyncFlow
// ============================================================
class SyncFlow {
	public:
	// GUI再描画要求コールバック
	std::function<void()> on_update;
	// ping送信ごとの軽量進捗コールバック（プロパティ再構築不要な更新用）
	std::function<void()> on_progress;
	// 各CH計測完了通知
	std::function<void(int ch, LatencyResult)> on_ch_measured;
	// マスター遅延書き込み要求
	std::function<void(double master_ms)> on_apply_master;
	// 全CH分の proposed_delay 書き込み要求（Step1完了時）
	std::function<void(const FlowResult &)> on_apply_sub_delays;

	SyncFlow();

	void set_active_channels(int n);
	void set_ping_count(int n);

	FlowPhase  phase() const;
	FlowResult result() const;
	bool       busy() const;

	bool start_step1(ods::network::StreamRouter &router, const std::string &stream_id);
	bool retry_failed_step1(ods::network::StreamRouter &router);
	bool start_step3(const std::string &rtmp_url);
	bool apply_step3();
	void reset();

	private:
	void on_ch_result(int i, LatencyResult r);
	void compute_proposals();

	mutable std::mutex mtx_;
	FlowPhase          phase_      = FlowPhase::Idle;
	int                active_ch_  = MAX_SUB_CH;
	int                ping_count_ = DEFAULT_PING_COUNT;
	std::atomic<int>   pending_count_{0};
	std::atomic<int>   ping_sent_count_{0};
	FlowResult         result_;
	RtmpProber         prober_;
};

} // namespace ods::sync
