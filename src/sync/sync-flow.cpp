#include "sync/sync-flow.hpp"

#include "network/stream-router.hpp"

#include <algorithm>

namespace ods::sync {

	using namespace ods::core;
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
		return p == FlowPhase::WsMeasuring || p == FlowPhase::RtmpMeasuring;
	}

	bool SyncFlow::start_ws_measurement(StreamRouter &router, const std::string &stream_id) {
		int active = MAX_SUB_CH;
		{
			std::lock_guard<std::mutex> lk(mtx_);
			const bool                  can_restart_from_current_phase =
				phase_ == FlowPhase::Idle ||
				phase_ == FlowPhase::WsDone ||
				phase_ == FlowPhase::RtmpDone ||
				phase_ == FlowPhase::Complete;
			if (!can_restart_from_current_phase) return false;
			active = active_ch_;
		}
		reset();
		int connected_count = 0;
		{
			std::lock_guard<std::mutex> lk(mtx_);
			phase_ = FlowPhase::WsMeasuring;
			for (int i = 0; i < active; ++i) {
				result_.channels[i].ch        = i;
				result_.channels[i].connected = (router.client_count(i) > 0);
				result_.channels[i].measured  = false;
				if (result_.channels[i].connected) ++connected_count;
			}
			result_.ping_total_count = connected_count * ping_count_;
		}

		if (connected_count == 0) {
			std::lock_guard<std::mutex> lk(mtx_);
			phase_ = FlowPhase::Idle;
			return false;
		}

		router.on_any_ping_sent = [this](const std::string &, int, int) {
			++ping_sent_count_;
			if (on_progress) on_progress();
		};

		pending_count_ = connected_count;
		int ch_index   = 0;
		for (int i = 0; i < active; ++i) {
			if (!result_.channels[i].connected) continue;
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

	bool SyncFlow::retry_failed_channels(StreamRouter &router) {
		int retry_count = 0;
		int active      = MAX_SUB_CH;
		{
			std::lock_guard<std::mutex> lk(mtx_);
			if (phase_ != FlowPhase::WsDone) return false;
			active = active_ch_;

			for (int i = 0; i < active; ++i) {
				auto &ch     = result_.channels[i];
				ch.connected = (router.client_count(i) > 0);
				if (ch.connected && !ch.measured) ++retry_count;
			}
			if (retry_count == 0) return false;

			// 進捗表示は「成功済み + 今回の再計測完了数」で更新する。
			result_.completed_count  = result_.measured_count();
			result_.ping_total_count = retry_count * ping_count_;
			phase_                   = FlowPhase::WsMeasuring;
		}

		ping_sent_count_.store(0, std::memory_order_relaxed);
		router.on_any_ping_sent = [this](const std::string &, int, int) {
			++ping_sent_count_;
			if (on_progress) on_progress();
		};

		pending_count_ = retry_count;
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

	bool SyncFlow::start_rtmp_measurement(const std::string &rtmp_url) {
		{
			std::lock_guard<std::mutex> lk(mtx_);
			const bool                  can_restart_from_current_phase =
				phase_ == FlowPhase::WsDone ||
				phase_ == FlowPhase::RtmpDone ||
				phase_ == FlowPhase::Complete;
			if (!can_restart_from_current_phase) return false;
			if (rtmp_url.empty()) return false;
			phase_ = FlowPhase::RtmpMeasuring;
		}
		if (on_update) on_update();

		prober_.on_result = [this](RtmpProbeResult r) {
			bool should_auto_apply = false;
			{
				std::lock_guard<std::mutex> lk(mtx_);
				if (r.valid) {
					result_.rtmp_valid      = true;
					result_.rtmp_latency_ms = r.avg_latency_ms;
					should_auto_apply       = true;
				} else {
					result_.rtmp_valid = false;
					result_.rtmp_error = r.error_msg;
				}
				phase_ = FlowPhase::RtmpDone;
			}
			if (should_auto_apply && apply_rtmp_result()) {
				return;
			}
			if (on_update) on_update();
		};
		prober_.start(rtmp_url, 10, 300);
		return true;
	}

	bool SyncFlow::apply_rtmp_result() {
		int ms;
		{
			std::lock_guard<std::mutex> lk(mtx_);
			if (phase_ != FlowPhase::RtmpDone) return false;
			if (!result_.rtmp_valid) return false;
			ms     = result_.proposed_master_delay_ms();
			phase_ = FlowPhase::Complete;
		}
		if (on_apply_master) on_apply_master(ms);
		if (on_update) on_update();
		return true;
	}

	void SyncFlow::reset() {
		prober_.cancel();
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
		if (all_done && on_apply_sub_base_delays) on_apply_sub_base_delays(result_snapshot);
		if (on_ch_measured) on_ch_measured(i, r);
		if (on_update) on_update();
	}

} // namespace ods::sync
