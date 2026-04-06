/*
 * sync-flow.cpp
 */

#include "sync/sync-flow.hpp"
#include "network/stream-router.hpp"

#include <algorithm>

// ============================================================
// SyncFlow
// ============================================================

SyncFlow::SyncFlow() { reset(); }

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
    FlowResult r = result_;
    r.ping_sent_count = ping_sent_count_.load(std::memory_order_relaxed);
    return r;
}

bool SyncFlow::busy() const {
    auto p = phase();
    return p == FlowPhase::Step1_Measuring || p == FlowPhase::Step3_Measuring;
}

bool SyncFlow::start_step1(StreamRouter& router, const std::string& stream_id) {
    int active = MAX_SUB_CH;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        const bool can_restart_from_current_phase =
            phase_ == FlowPhase::Idle ||
            phase_ == FlowPhase::Step1_Done ||
            phase_ == FlowPhase::Step3_Done ||
            phase_ == FlowPhase::Complete;
        if (!can_restart_from_current_phase) return false;
        active = active_ch_;
    }
    reset();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        phase_ = FlowPhase::Step1_Measuring;
        for (int i = 0; i < active; ++i) {
            result_.channels[i].ch        = i;
            result_.channels[i].connected = (router.client_count(i) > 0);
            result_.channels[i].measured  = false;
            if (result_.channels[i].connected) ++result_.connected_count;
        }
        result_.ping_total_count = result_.connected_count * ping_count_;
    }

    if (result_.connected_count == 0) {
        std::lock_guard<std::mutex> lk(mtx_);
        phase_ = FlowPhase::Idle;
        return false;
    }

    router.on_any_ping_sent = [this](const std::string&, int, int) {
        ++ping_sent_count_;
        if (on_progress) on_progress();
    };

    pending_count_ = result_.connected_count;
    int ch_index = 0;
    for (int i = 0; i < active; ++i) {
        if (!result_.channels[i].connected) continue;
        router.set_on_latency_result(i,
            [this, i](const std::string&, int, LatencyResult r) {
                on_ch_result(i, r);
            });
        router.start_measurement(i, ping_count_, PING_INTV_MS, ch_index * PING_INTV_MS);
        ++ch_index;
    }
    if (on_update) on_update();
    return true;
}

bool SyncFlow::retry_failed_step1(StreamRouter& router) {
    int retry_count = 0;
    int active = MAX_SUB_CH;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (phase_ != FlowPhase::Step1_Done) return false;
        active = active_ch_;

        for (int i = 0; i < active; ++i) {
            auto& ch = result_.channels[i];
            ch.connected = (router.client_count(i) > 0);
            if (ch.connected && !ch.measured) ++retry_count;
        }
        if (retry_count == 0) return false;

        // 進捗表示は「成功済み + 今回の再計測完了数」で更新する。
        result_.completed_count = result_.measured_count;
        result_.ping_total_count = retry_count * ping_count_;
        phase_ = FlowPhase::Step1_Measuring;
    }

    ping_sent_count_.store(0, std::memory_order_relaxed);
    router.on_any_ping_sent = [this](const std::string&, int, int) {
        ++ping_sent_count_;
        if (on_progress) on_progress();
    };

    pending_count_ = retry_count;
    int ch_index = 0;
    for (int i = 0; i < active; ++i) {
        auto& ch = result_.channels[i];
        if (!ch.connected || ch.measured) continue;
        router.set_on_latency_result(i,
            [this, i](const std::string&, int, LatencyResult r) {
                on_ch_result(i, r);
            });
        router.start_measurement(i, ping_count_, PING_INTV_MS, ch_index * PING_INTV_MS);
        ++ch_index;
    }
    if (on_update) on_update();
    return true;
}

bool SyncFlow::start_step3(const std::string& rtmp_url) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        const bool can_restart_from_current_phase =
            phase_ == FlowPhase::Step1_Done ||
            phase_ == FlowPhase::Step3_Done ||
            phase_ == FlowPhase::Complete;
        if (!can_restart_from_current_phase) return false;
        if (rtmp_url.empty()) return false;
        phase_ = FlowPhase::Step3_Measuring;
    }
    if (on_update) on_update();

    prober_.on_result = [this](RtmpProbeResult r) {
        bool should_auto_apply = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (r.valid) {
                result_.rtmp_valid      = true;
                result_.rtmp_latency_ms = r.avg_latency_ms;
                result_.master_delay_ms = result_.max_latency_ms + r.avg_latency_ms;
                should_auto_apply = true;
            } else {
                result_.rtmp_valid = false;
                result_.rtmp_error = r.error_msg;
            }
            phase_ = FlowPhase::Step3_Done;
        }
        if (should_auto_apply && apply_step3()) {
            return;
        }
        if (on_update) on_update();
    };
    prober_.start(rtmp_url, 10, 300);
    return true;
}

bool SyncFlow::apply_step3() {
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

void SyncFlow::reset() {
    prober_.cancel();
    std::lock_guard<std::mutex> lk(mtx_);
    phase_            = FlowPhase::Idle;
    pending_count_    = 0;
    ping_sent_count_  = 0;
    result_           = FlowResult{};
}

// ============================================================
// private
// ============================================================

void SyncFlow::on_ch_result(int i, LatencyResult r) {
    bool all_done = false;
    FlowResult result_snapshot;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (i < 0 || i >= active_ch_) return;
        auto& ch = result_.channels[i];
        ch.measured   = r.valid;
        ch.one_way_latency_ms = r.valid ? r.avg_latency_ms : 0.0;
        ++result_.completed_count;
        if (r.valid) ++result_.measured_count;

        if (--pending_count_ == 0) {
            compute_proposals();
            phase_ = FlowPhase::Step1_Done;
            all_done = true;
            result_snapshot = result_;
        }
    }
    if (all_done && on_apply_sub_delays) on_apply_sub_delays(result_snapshot);
    if (on_ch_measured) on_ch_measured(i, r);
    if (on_update) on_update();
}

void SyncFlow::compute_proposals() {
    double max_ow = 0.0;
    for (int i = 0; i < active_ch_; ++i) {
        auto& ch = result_.channels[i];
        if (ch.measured && ch.one_way_latency_ms > max_ow)
            max_ow = ch.one_way_latency_ms;
    }
    result_.max_latency_ms = max_ow;

    for (int i = 0; i < active_ch_; ++i) {
        auto& ch = result_.channels[i];
        ch.proposed_delay = ch.measured ? (max_ow - ch.one_way_latency_ms) : 0.0;
    }
}
