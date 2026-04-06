#include "plugin-main-update.hpp"

#include <cmath>
#include <mutex>
#include <string>

#include "constants.hpp"
#include "plugin-main-audio-processing.hpp"
#include "plugin-main-obs-services.hpp"
#include "plugin-main-settings-helpers.hpp"
#include "plugin-main-state.hpp"
#include "plugin-main-sub-settings.hpp"
#include "plugin-main-utils.hpp"

namespace plugin_main_update {

namespace {

using plugin_main_utils::clamp_sub_ch_count;
using plugin_main_utils::normalize_opus_sample_rate;
using plugin_main_utils::normalize_quantization_bits;
using plugin_main_utils::normalize_pcm_downsample_ratio;
using plugin_main_utils::normalize_playback_buffer_ms;
using plugin_main_utils::sanitize_stream_id;
using plugin_main_utils::generate_stream_id;
using plugin_main_sub_settings::make_sub_delay_key;
using plugin_main_sub_settings::make_sub_adjust_key;
using plugin_main_sub_settings::make_sub_memo_key;
using plugin_main_sub_settings::make_sub_code_key;
using plugin_main_sub_settings::calc_sub_delay_raw_value_ms;
using plugin_main_sub_settings::calc_effective_sub_delay_value_ms;

constexpr float kSubAdjustMinMs = -500.0f;
constexpr float kSubAdjustMaxMs = 500.0f;

class SettingsApplier {
    DelayStreamData* d_;
    obs_data_t*      s_;
public:
    SettingsApplier(DelayStreamData* d, obs_data_t* s) : d_(d), s_(s) {}

    void apply_all() {
        bool stream_id_has_user_value = obs_data_has_user_value(s_, "stream_id");
        bool reset_to_defaults =
            d_->create_done.load(std::memory_order_relaxed) &&
            d_->prev_stream_id_has_user_value &&
            !stream_id_has_user_value;

        if (reset_to_defaults) handle_defaults_reset();

        apply_basic_flags();
        apply_sub_channel_count();
        apply_audio_codec_settings();
        apply_stream_endpoint_settings();

        bool effective_delay_changed = apply_delay_settings();

        d_->rtmp_url_auto.store(obs_data_get_bool(s_, "rtmp_url_auto"), std::memory_order_relaxed);
        apply_ping_count();

        if (effective_delay_changed) {
            d_->request_props_refresh("ds_update.effective_changed");
        }
        d_->prev_stream_id_has_user_value = obs_data_has_user_value(s_, "stream_id");
    }

private:
    void handle_defaults_reset() {
        if (d_->router_running.load(std::memory_order_relaxed)) {
            d_->router.stop();
            d_->router_running.store(false, std::memory_order_relaxed);
            blog(LOG_INFO, "[obs-delay-stream] WebSocket server stopped by defaults reset");
        }
        TunnelState ts = d_->tunnel.state();
        if (ts == TunnelState::Starting || ts == TunnelState::Running) {
            d_->tunnel.stop();
            blog(LOG_INFO, "[obs-delay-stream] Tunnel stopped by defaults reset");
        }
    }

    void apply_basic_flags() {
        bool delay_disable = obs_data_get_bool(s_, "delay_disable");
        d_->enabled.store(!delay_disable);
        bool paused = obs_data_get_bool(s_, "ws_send_paused");
        d_->ws_send_enabled.store(!paused);
    }

    void apply_sub_channel_count() {
        int current = d_->sub_ch_count;
        bool has_sub_ch_count = obs_data_has_user_value(s_, "sub_ch_count");
        int raw_v = has_sub_ch_count ? (int)obs_data_get_int(s_, "sub_ch_count") : current;
        int v = raw_v;
        if (v <= 0) v = (current > 0) ? current : 1;
        int clamped = clamp_sub_ch_count(v);
        if (!has_sub_ch_count || clamped != v) {
            obs_data_set_int(s_, "sub_ch_count", clamped);
        }
        bool changed = (clamped != current);
        if (changed || !has_sub_ch_count || raw_v != clamped) {
            blog(LOG_INFO,
                 "[obs-delay-stream] ds_update sub_ch_count current=%d has_user=%d raw=%d normalized=%d clamped=%d",
                 current, has_sub_ch_count ? 1 : 0, raw_v, v, clamped);
        }
        d_->sub_ch_count = clamped;
        d_->router.set_active_channels(clamped);
        d_->flow.set_active_channels(clamped);
        if (changed) d_->flow.reset();
    }

    void apply_audio_codec_settings() {
        int audio_codec = (int)obs_data_get_int(s_, "audio_codec");
        d_->router.set_audio_codec(audio_codec);
        {
            int bitrate = (int)obs_data_get_int(s_, "opus_bitrate_kbps");
            if (bitrate < 6) {
                bitrate = 6;
                obs_data_set_int(s_, "opus_bitrate_kbps", bitrate);
            } else if (bitrate > 510) {
                bitrate = 510;
                obs_data_set_int(s_, "opus_bitrate_kbps", bitrate);
            }
            d_->router.set_opus_bitrate_kbps(bitrate);
        }
        {
            int sample_rate = normalize_opus_sample_rate(
                (int)obs_data_get_int(s_, "opus_sample_rate"));
            if (sample_rate != (int)obs_data_get_int(s_, "opus_sample_rate")) {
                obs_data_set_int(s_, "opus_sample_rate", sample_rate);
            }
            d_->router.set_opus_target_sample_rate(sample_rate);
        }
        {
            int quant_bits = normalize_quantization_bits(
                (int)obs_data_get_int(s_, "quantization_bits"));
            if (quant_bits != (int)obs_data_get_int(s_, "quantization_bits")) {
                obs_data_set_int(s_, "quantization_bits", quant_bits);
            }
            d_->router.set_audio_quantization_bits(quant_bits);
        }
        d_->router.set_audio_mono(obs_data_get_bool(s_, "audio_mono"));
        {
            int ratio = normalize_pcm_downsample_ratio(
                (int)obs_data_get_int(s_, "pcm_downsample_ratio"));
            if (ratio != (int)obs_data_get_int(s_, "pcm_downsample_ratio"))
                obs_data_set_int(s_, "pcm_downsample_ratio", ratio);
            d_->router.set_pcm_downsample_ratio(ratio);
        }
        {
            int raw_pb_ms = (int)obs_data_get_int(s_, "playback_buffer_ms");
            int pb_ms = normalize_playback_buffer_ms(raw_pb_ms);
            if (pb_ms != raw_pb_ms) {
                obs_data_set_int(s_, "playback_buffer_ms", pb_ms);
            }
            d_->playback_buffer_ms = pb_ms;
            d_->router.set_playback_buffer_ms(pb_ms);
        }
    }

    void apply_stream_endpoint_settings() {
        const char* raw = obs_data_get_string(s_, "stream_id");
        std::string sid = raw ? sanitize_stream_id(raw) : "";
        if (sid.empty()) {
            if (!d_->sid_autofill_guard.exchange(true)) {
                sid = generate_stream_id(12);
                obs_data_set_string(s_, "stream_id", sid.c_str());
                d_->sid_autofill_guard.store(false);
            } else {
                sid = raw ? sanitize_stream_id(raw) : "";
            }
        }
        d_->set_stream_id(sid);
        d_->router.set_stream_id(sid);
        plugin_main_obs_services::maybe_autofill_rtmp_url(s_, true);
        plugin_main_settings_helpers::maybe_fill_cloudflared_path_from_auto(d_->context);

        {
            int ws_port = (int)obs_data_get_int(s_, "ws_port");
            if (ws_port < 1 || ws_port > 65535) {
                ws_port = WS_PORT;
                obs_data_set_int(s_, "ws_port", ws_port);
            }
            d_->ws_port.store(ws_port, std::memory_order_relaxed);
        }

        const char* hip = obs_data_get_string(s_, "host_ip_manual");
        d_->set_host_ip(hip);
    }

    bool apply_delay_settings() {
        d_->master_delay_ms = (float)obs_data_get_double(s_, "master_delay_ms");
        d_->master_buf.set_delay_ms(d_->enabled.load() ? (uint32_t)d_->master_delay_ms : 0);

        const float prev_sub_offset = d_->sub_offset_ms;
        d_->sub_offset_ms = (float)obs_data_get_double(s_, "sub_offset_ms");

        bool effective_delay_changed = false;
        for (int i = 0; i < MAX_SUB_CH; ++i) {
            const float prev_delay = d_->sub[i].delay_ms;
            const float prev_adjust = d_->sub[i].adjust_ms;

            const auto delay_key = make_sub_delay_key(i);
            d_->sub[i].delay_ms = (float)obs_data_get_double(s_, delay_key.data());

            const auto adjust_key = make_sub_adjust_key(i);
            float adjust = (float)obs_data_get_double(s_, adjust_key.data());
            if (adjust < kSubAdjustMinMs) {
                adjust = kSubAdjustMinMs;
                obs_data_set_double(s_, adjust_key.data(), adjust);
            } else if (adjust > kSubAdjustMaxMs) {
                adjust = kSubAdjustMaxMs;
                obs_data_set_double(s_, adjust_key.data(), adjust);
            }
            d_->sub[i].adjust_ms = adjust;

            const auto memo_key = make_sub_memo_key(i);
            const char* memo = obs_data_get_string(s_, memo_key.data());
            d_->router.set_sub_memo(i, memo ? memo : "");

            const auto code_key = make_sub_code_key(i);
            const char* code_raw = obs_data_get_string(s_, code_key.data());
            std::string code = code_raw ? code_raw : "";
            if (code.empty()) {
                code = generate_stream_id(8);
                obs_data_set_string(s_, code_key.data(), code.c_str());
            }
            d_->router.set_sub_code(i, code);
            plugin_main_audio_processing::apply_sub_delay_to_buffer(d_, i);

            const float prev_raw =
                calc_sub_delay_raw_value_ms(prev_delay, prev_adjust, prev_sub_offset);
            const float new_raw =
                calc_sub_delay_raw_value_ms(d_->sub[i].delay_ms, d_->sub[i].adjust_ms, d_->sub_offset_ms);
            const float prev_effective =
                calc_effective_sub_delay_value_ms(prev_delay, prev_adjust, prev_sub_offset);
            const float new_effective =
                calc_effective_sub_delay_value_ms(d_->sub[i].delay_ms, d_->sub[i].adjust_ms, d_->sub_offset_ms);
            if (std::fabs(prev_effective - new_effective) > 0.01f) {
                d_->router.notify_apply_delay(i, new_effective, "manual_adjust");
                effective_delay_changed = true;
            } else if (std::fabs(prev_raw - new_raw) > 0.01f) {
                effective_delay_changed = true;
            }
        }

        return effective_delay_changed;
    }

    void apply_ping_count() {
        int pc = (int)obs_data_get_int(s_, "ping_count");
        if (pc != 10 && pc != 20 && pc != 30 && pc != 40 && pc != 50) {
            pc = DEFAULT_PING_COUNT;
            obs_data_set_int(s_, "ping_count", pc);
        }
        d_->ping_count_setting.store(pc, std::memory_order_relaxed);
        d_->flow.set_ping_count(pc);
    }
};

} // namespace

void apply_settings(DelayStreamData* d, obs_data_t* settings) {
    if (!d || !settings) return;

    if (d->is_duplicate_instance) {
        d->enabled.store(false, std::memory_order_relaxed);
        d->ws_send_enabled.store(false, std::memory_order_relaxed);
        d->prev_stream_id_has_user_value = obs_data_has_user_value(settings, "stream_id");
        return;
    }

    SettingsApplier(d, settings).apply_all();
}

} // namespace plugin_main_update
