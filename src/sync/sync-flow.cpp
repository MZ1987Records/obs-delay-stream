#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "sync/sync-flow.hpp"

#include "plugin/plugin-services.hpp"
#include "plugin/plugin-state.hpp"
#include "network/stream-router.hpp"

#include <algorithm>
#include <chrono>

namespace ods::sync {

	using namespace ods::core;
	using ods::plugin::DelayStreamData;
	using ods::network::StreamRouter;

	// ============================================================
	// SyncFlow
	// ============================================================

	SyncFlow::SyncFlow() {
		reset();
	}

	void SyncFlow::set_active_channels(int n) {
		if (n < 1) n = 1;
		if (n > MAX_SUB_CH) n = MAX_SUB_CH;
		std::lock_guard<std::mutex> lk(mtx_);
		active_ch_ = n;
	}

	void SyncFlow::set_ping_count(int n) {
		if (n < 1) n = 1;
		std::lock_guard<std::mutex> lk(mtx_);
		ping_count_ = n;
	}

	FlowPhase SyncFlow::phase() const {
		std::lock_guard<std::mutex> lk(mtx_);
		return phase_;
	}

	FlowResult SyncFlow::result() const {
		std::lock_guard<std::mutex> lk(mtx_);
		FlowResult                  r = result_;
		r.ping_sent_count             = ping_sent_count_.load(std::memory_order_relaxed);
		return r;
	}

	bool SyncFlow::busy() const {
		auto p = phase();
		return p == FlowPhase::WsMeasuring ||
			   p == FlowPhase::RtspE2eMeasuring;
	}

	bool SyncFlow::start_ws_measurement(StreamRouter      &router,
										const std::string &stream_id,
										const int         *skip_measured_ms) {
		int active = MAX_SUB_CH;
		{
			std::lock_guard<std::mutex> lk(mtx_);
			const bool                  can_restart_from_current_phase =
				phase_ == FlowPhase::Idle ||
				phase_ == FlowPhase::WsDone ||
				phase_ == FlowPhase::RtspE2eDone ||
				phase_ == FlowPhase::Complete;
			if (!can_restart_from_current_phase) return false;
			active = active_ch_;
		}
		reset();
		int measure_count = 0;
		{
			std::lock_guard<std::mutex> lk(mtx_);
			phase_ = FlowPhase::WsMeasuring;
			for (int i = 0; i < active; ++i) {
				auto &ch     = result_.channels[i];
				ch.ch        = i;
				ch.connected = (router.client_count(i) > 0);
				ch.measured  = false;

				// スキップ対象: 接続中かつ計測済み値が渡されたチャンネル
				if (ch.connected && skip_measured_ms && skip_measured_ms[i] >= 0) {
					ch.measured           = true;
					ch.one_way_latency_ms = static_cast<double>(skip_measured_ms[i]);
					++result_.completed_count;
				} else if (ch.connected) {
					++measure_count;
				}
			}
			result_.ping_total_count = measure_count * ping_count_;
		}

		if (measure_count == 0) {
			// 計測対象がない場合は即完了にする。
			std::lock_guard<std::mutex> lk(mtx_);
			if (result_.connected_count() == 0) {
				phase_ = FlowPhase::Idle;
				return false;
			}
			phase_ = FlowPhase::WsDone;
			if (on_ws_measured) on_ws_measured(result_);
			if (on_update) on_update();
			return true;
		}

		router.on_any_ping_sent = [this](const std::string &, int, int) {
			++ping_sent_count_;
			if (on_progress) on_progress();
		};

		pending_count_ = measure_count;
		int ch_index   = 0;
		for (int i = 0; i < active; ++i) {
			auto &ch = result_.channels[i];
			if (!ch.connected || ch.measured) continue;
			router.set_on_latency_result(i,
										 [this, i](const std::string &, int, LatencyResult r) {
											 on_ch_result(i, r);
										 });
			router.start_measurement(i, ping_count_, PING_INTV_MS, ch_index * PING_INTV_MS);
			++ch_index;
		}
		if (on_update) on_update();
		return true;
	}

	bool SyncFlow::start_rtsp_e2e_measurement(const std::string &rtsp_url,
											  const std::string &ffmpeg_path,
											  DelayStreamData   &audio_data) {
		{
			std::lock_guard<std::mutex> lk(mtx_);
			const bool                  can_restart_from_current_phase =
				phase_ == FlowPhase::Idle ||
				phase_ == FlowPhase::WsDone ||
				phase_ == FlowPhase::RtspE2eDone ||
				phase_ == FlowPhase::Complete;
			if (!can_restart_from_current_phase) return false;
		}

		std::string error_msg;
		if (rtsp_url.empty()) {
			error_msg = "RTSP URL が未設定です。";
		} else if (!ods::plugin::is_obs_streaming_active()) {
			error_msg = "OBS 配信が開始されていません。";
		}

		if (!error_msg.empty()) {
			{
				std::lock_guard<std::mutex> lk(mtx_);
				result_.rtsp_e2e_valid          = false;
				result_.rtsp_e2e_error          = error_msg;
				result_.rtsp_e2e_latency_ms     = 0.0;
				result_.rtsp_e2e_min_latency_ms = 0.0;
				result_.rtsp_e2e_max_latency_ms = 0.0;
				result_.rtsp_e2e_completed_sets = 0;
				result_.rtsp_e2e_total_sets     = RTSP_E2E_MEASURE_SETS_DEFAULT;
				phase_                          = FlowPhase::RtspE2eDone;
			}
			if (on_update) on_update();
			return false;
		}

		{
			std::lock_guard<std::mutex> lk(mtx_);
			result_.rtsp_e2e_valid = false;
			result_.rtsp_e2e_error.clear();
			result_.rtsp_e2e_latency_ms     = 0.0;
			result_.rtsp_e2e_min_latency_ms = 0.0;
			result_.rtsp_e2e_max_latency_ms = 0.0;
			result_.rtsp_e2e_completed_sets = 0;
			result_.rtsp_e2e_total_sets     = RTSP_E2E_MEASURE_SETS_DEFAULT;
			phase_                          = FlowPhase::RtspE2eMeasuring;
		}
		if (on_update) on_update();
		if (on_progress) on_progress();

		audio_data.inject_impulse.store(false, std::memory_order_release);
		audio_data.rtsp_e2e_measure.set_cached_url(rtsp_url);
		prober_e2e_.on_ready = [this, &audio_data]() {
			audio_data.inject_impulse.store(true, std::memory_order_release);
			prober_e2e_.notify_impulse_sent(std::chrono::steady_clock::now());
		};
		prober_e2e_.on_progress = [this](int completed_sets, int total_sets) {
			{
				std::lock_guard<std::mutex> lk(mtx_);
				result_.rtsp_e2e_completed_sets = completed_sets;
				result_.rtsp_e2e_total_sets     = total_sets;
			}
			if (on_progress) on_progress();
		};
		prober_e2e_.on_result = [this, &audio_data](RtspE2eResult r) {
			bool should_auto_apply = false;
			audio_data.rtsp_e2e_measure.apply_result(r);
			{
				std::lock_guard<std::mutex> lk(mtx_);
				result_.rtsp_e2e_valid          = r.valid;
				result_.rtsp_e2e_latency_ms     = r.latency_ms;
				result_.rtsp_e2e_min_latency_ms = r.min_latency_ms;
				result_.rtsp_e2e_max_latency_ms = r.max_latency_ms;
				result_.rtsp_e2e_error          = r.error_msg;
				if (result_.rtsp_e2e_total_sets > 0 && result_.rtsp_e2e_valid) {
					result_.rtsp_e2e_completed_sets = result_.rtsp_e2e_total_sets;
				}
				should_auto_apply = result_.rtsp_e2e_valid;
				phase_            = FlowPhase::RtspE2eDone;
			}
			if (should_auto_apply && apply_rtsp_e2e_result()) {
				return;
			}
			if (on_progress) on_progress();
			if (on_update) on_update();
		};
		if (!prober_e2e_.start(rtsp_url, ffmpeg_path)) {
			{
				std::lock_guard<std::mutex> lk(mtx_);
				result_.rtsp_e2e_valid          = false;
				result_.rtsp_e2e_error          = "RTSP E2E 計測の開始に失敗しました。";
				result_.rtsp_e2e_latency_ms     = 0.0;
				result_.rtsp_e2e_min_latency_ms = 0.0;
				result_.rtsp_e2e_max_latency_ms = 0.0;
				phase_                          = FlowPhase::RtspE2eDone;
			}
			if (on_update) on_update();
			return false;
		}
		return true;
	}

	bool SyncFlow::apply_rtsp_e2e_result() {
		int ms;
		{
			std::lock_guard<std::mutex> lk(mtx_);
			if (phase_ != FlowPhase::RtspE2eDone) return false;
			if (!result_.rtsp_e2e_valid) return false;
			ms     = result_.rtsp_e2e_ms();
			phase_ = FlowPhase::Complete;
		}
		if (on_rtsp_e2e_measured) on_rtsp_e2e_measured(ms);
		if (on_update) on_update();
		return true;
	}

	void SyncFlow::reset() {
		prober_e2e_.cancel();
		prober_e2e_.on_ready    = nullptr;
		prober_e2e_.on_progress = nullptr;
		prober_e2e_.on_result   = nullptr;
		std::lock_guard<std::mutex> lk(mtx_);
		phase_           = FlowPhase::Idle;
		pending_count_   = 0;
		ping_sent_count_ = 0;
		result_          = FlowResult{};
	}

	// ============================================================
	// private
	// ============================================================

	void SyncFlow::on_ch_result(int i, LatencyResult r) {
		bool       all_done = false;
		FlowResult result_snapshot;
		{
			std::lock_guard<std::mutex> lk(mtx_);
			if (i < 0 || i >= active_ch_) return;
			auto &ch              = result_.channels[i];
			ch.measured           = r.valid;
			ch.one_way_latency_ms = r.valid ? r.avg_latency_ms : 0.0;
			++result_.completed_count;

			if (--pending_count_ == 0) {
				phase_          = FlowPhase::WsDone;
				all_done        = true;
				result_snapshot = result_;
			}
		}
		if (all_done && on_ws_measured) on_ws_measured(result_snapshot);
		if (on_ch_measured) on_ch_measured(i, r);
		if (on_update) on_update();
	}

} // namespace ods::sync
