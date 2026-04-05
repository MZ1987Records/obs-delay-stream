#include "plugin-main-properties-ui.hpp"

#include <cstdio>
#include <string>
#include <mutex>

#include "color-buttons-widget.hpp"
#include "constants.hpp"
#include "delay-table-widget.hpp"
#include "plugin-main-config.hpp"
#include "plugin-main-obs-services.hpp"
#include "plugin-main-props-refresh.hpp"
#include "plugin-main-settings-helpers.hpp"
#include "plugin-main-state.hpp"
#include "plugin-main-sub-settings.hpp"
#include "plugin-main-utils.hpp"
#include "stepper-widget.hpp"
#include "text-button-widget.hpp"

#define T_(s) obs_module_text(s)

namespace plugin_main_properties_ui {

namespace {

using plugin_main_utils::extract_host_from_url;
using plugin_main_sub_settings::make_sub_memo_key;

static constexpr int64_t REQUIRED_AUDIO_SYNC_OFFSET_NS = -950LL * 1000000LL;

void request_properties_refresh(DelayStreamData* d, const char* reason = nullptr) {
    if (!d) return;
    plugin_main_props_refresh::props_refresh_request(
        d->context,
        d->create_done.load(std::memory_order_acquire),
        d->destroying.load(std::memory_order_acquire),
        d->get_props_depth.load(std::memory_order_acquire),
        reason);
}

bool cb_rtmp_url_auto_changed(void* priv, obs_properties_t* props, obs_property_t*, obs_data_t* settings) {
    auto* d = static_cast<DelayStreamData*>(priv);
    if (!d || !settings) return false;
    bool auto_new = obs_data_get_bool(settings, "rtmp_url_auto");
    d->rtmp_url_auto.store(auto_new, std::memory_order_relaxed);
    if (auto_new) {
        plugin_main_obs_services::maybe_autofill_rtmp_url(settings, true);
    }
    if (props) {
        if (auto* url_p = obs_properties_get(props, "rtmp_url")) {
            obs_property_set_enabled(url_p, !auto_new);
        }
    }
    return true;
}

bool cb_ws_server_start(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    if (!d || d->router_running.load()) return false;
    int ws_port = d->ws_port.load(std::memory_order_relaxed);
    if (d->router.start((uint16_t)ws_port)) {
        d->router_running.store(true);
        blog(LOG_INFO, "[obs-delay-stream] WebSocket server started on port %d", ws_port);
    } else {
        blog(LOG_ERROR, "[obs-delay-stream] WebSocket server FAILED to start on port %d", ws_port);
    }
    request_properties_refresh(d, "cb_ws_server_start");
    return false;
}

bool cb_ws_server_stop(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    if (!d || !d->router_running.load()) return false;
    d->router.stop();
    d->router_running.store(false);
    blog(LOG_INFO, "[obs-delay-stream] WebSocket server stopped");
    request_properties_refresh(d, "cb_ws_server_stop");
    return false;
}

bool cb_tunnel_start(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    if (!d || !d->context) return false;
    obs_data_t* s = obs_source_get_settings(d->context);
    if (!s) return false;
    const char* exe = obs_data_get_string(s, "cloudflared_exe_path");
    obs_data_release(s);
    int ws_port = d->ws_port.load(std::memory_order_relaxed);
    d->tunnel.start(exe ? exe : "", ws_port);
    request_properties_refresh(d, "cb_tunnel_start");
    return false;
}

bool cb_tunnel_stop(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    if (!d) return false;
    d->tunnel.stop();
    request_properties_refresh(d, "cb_tunnel_stop");
    return false;
}

bool cb_flow_start(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    if (!d) return false;
    std::string sid = d->get_stream_id();
    if (sid.empty()) return false;
    d->flow.start_step1(d->router, sid);
    return false;
}

bool cb_flow_retry_failed(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    if (!d) return false;
    d->flow.retry_failed_step1(d->router);
    return false;
}

bool cb_flow_start_step3(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    if (!d) return false;
    std::string url = plugin_main_settings_helpers::resolve_rtmp_url_from_source(d->context);
    d->flow.start_step3(url);
    return false;
}

bool cb_flow_reset(obs_properties_t*, obs_property_t*, void* priv) {
    auto* d = static_cast<DelayStreamData*>(priv);
    if (!d) return false;
    d->flow.reset();
    request_properties_refresh(d, "cb_flow_reset");
    return false;
}

bool cb_stream_id_changed(void*, obs_properties_t* props, obs_property_t*, obs_data_t* settings) {
    if (!props || !settings) return false;
    const char* sid = obs_data_get_string(settings, "stream_id");
    bool has_sid = (sid && *sid);
    if (auto* p = obs_properties_get(props, "ws_server_start_btn")) {
        obs_property_set_enabled(p, has_sid);
    }
    if (auto* p = obs_properties_get(props, "ws_server_start_note_sid")) {
        obs_property_set_visible(p, !has_sid);
    }
    return true;
}

bool cb_audio_codec_changed(void* priv, obs_properties_t* props, obs_property_t*, obs_data_t* settings) {
    auto* d = static_cast<DelayStreamData*>(priv);
    plugin_main_config::apply_codec_option_visibility(props, settings);
    // OBSはtrueを返すとパネルを再描画する。ちらつき抑制のためフリーズし、
    // スクロール位置を保存・復元しつつカスタムウィジェットを再注入する。
    plugin_main_props_refresh::props_ui_with_preserved_scroll([d]() {
        if (!d || !d->context) return;
        schedule_stepper_inject(d->context);
        schedule_text_button_inject(d->context);
        schedule_color_button_row_inject(d->context);
        schedule_delay_table_inject(d->context);
    });
    return true;
}

void add_flow_rtmp_measure_section(obs_properties_t* props, DelayStreamData* d) {
    if (!props || !d) return;

    FlowPhase phase = d->flow.phase();
    FlowResult res  = d->flow.result();
    const bool is_complete        = (phase == FlowPhase::Complete);
    const bool is_step1_done      = (phase == FlowPhase::Step1_Done);
    const bool is_step3_measuring = (phase == FlowPhase::Step3_Measuring);
    const bool is_step3_done      = (phase == FlowPhase::Step3_Done);
    const bool can_start_step3 =
        is_step1_done || is_step3_done || is_complete;

    const ObsColorButtonSpec flow_rtmp_measure_buttons[] = {
        {
            "flow_rtmp_measure_start_btn",
            T_("FlowMeasureStartShort"),
            cb_flow_start_step3,
            d,
            (!is_step3_measuring && can_start_step3),
            nullptr,
            nullptr,
        },
        {
            "flow_rtmp_measure_stop_btn",
            T_("FlowMeasureStopShort"),
            cb_flow_reset,
            d,
            is_step3_measuring,
            nullptr,
            nullptr,
        },
    };
    obs_properties_add_color_button_row(
        props, "flow_rtmp_measure_controls_row", T_("FlowRtmpMeasureLabel"), flow_rtmp_measure_buttons,
        sizeof(flow_rtmp_measure_buttons) / sizeof(flow_rtmp_measure_buttons[0]));

    char step3_status[512];
    if (is_step3_measuring) {
        snprintf(step3_status, sizeof(step3_status), "%s", T_("FlowRtmpMeasureProgress"));
    } else if ((is_step3_done || is_complete) && res.rtmp_valid) {
        snprintf(step3_status, sizeof(step3_status), T_("FlowRtmpMeasureResultFmt"),
                 res.rtmp_latency_ms, res.max_latency_ms, res.master_delay_ms);
    } else if (is_step3_done && !res.rtmp_valid) {
        snprintf(step3_status, sizeof(step3_status), T_("FlowRtmpFailedFmt"), res.rtmp_error.c_str());
    } else {
        snprintf(step3_status, sizeof(step3_status), "%s", T_("FlowNotMeasured"));
    }
    obs_properties_add_text(props, "flow_s3_status", step3_status, OBS_TEXT_INFO);
}

void build_flow_panel(obs_properties_t* props, DelayStreamData* d) {
    if (!props || !d) return;
    obs_properties_add_text(props, "flow_desc",
        T_("FlowDesc"),
        OBS_TEXT_INFO);

    FlowPhase phase = d->flow.phase();
    FlowResult res  = d->flow.result();
    int sub_count = d->sub_ch_count;

    const bool is_complete        = (phase == FlowPhase::Complete);
    const bool is_step1_measuring = (phase == FlowPhase::Step1_Measuring);
    const bool is_step1_done      = (phase == FlowPhase::Step1_Done);
    const bool is_step3_measuring = (phase == FlowPhase::Step3_Measuring);
    const bool is_step3_done      = (phase == FlowPhase::Step3_Done);
    const bool is_step1_done_or_later =
        is_step1_done || is_step3_measuring || is_step3_done || is_complete;

    obs_data_t* s = obs_source_get_settings(d->context);
    auto format_sub_name = [s](int ch) -> std::string {
        if (!s) return "Ch." + std::to_string(ch + 1);
        const auto memo_key = make_sub_memo_key(ch);
        const char* memo = obs_data_get_string(s, memo_key.data());
        if (memo && *memo) return std::string(memo);
        return "Ch." + std::to_string(ch + 1);
    };

    std::string connected_names;
    std::string disconnected_names;
    int connected_count = 0;
    int disconnected_count = 0;
    for (int i = 0; i < sub_count; ++i) {
        std::string name = format_sub_name(i);
        if (d->router.client_count(i) > 0) {
            if (!connected_names.empty()) connected_names += " ";
            connected_names += name;
            ++connected_count;
        } else {
            if (!disconnected_names.empty()) disconnected_names += " ";
            disconnected_names += name;
            ++disconnected_count;
        }
    }

    if (connected_count == 0) connected_names = T_("FlowNone");
    if (disconnected_count == 0) disconnected_names = T_("FlowNone");

    std::string status_text = std::string(T_("FlowConnected")) + connected_names +
                              "\n" + T_("FlowDisconnected") + disconnected_names;
    obs_properties_add_text(props, "flow_connected", status_text.c_str(), OBS_TEXT_INFO);

    const bool flow_measuring = d->flow.busy();
    const ObsColorButtonSpec flow_measure_buttons[] = {
        {
            "flow_measure_start_btn",
            T_("FlowMeasureStartShort"),
            cb_flow_start,
            d,
            (!flow_measuring && connected_count > 0),
            nullptr,
            nullptr,
        },
        {
            "flow_measure_stop_btn",
            T_("FlowMeasureStopShort"),
            cb_flow_reset,
            d,
            flow_measuring,
            nullptr,
            nullptr,
        },
    };
    obs_properties_add_color_button_row(
        props, "flow_measure_controls_row", T_("FlowMeasureLabel"), flow_measure_buttons,
        sizeof(flow_measure_buttons) / sizeof(flow_measure_buttons[0]));

    std::string step1_status_text;
    if (is_step1_measuring) {
        char buf[256];
        snprintf(buf, sizeof(buf), T_("FlowMeasureProgressFmt"),
                 res.completed_count, res.connected_count);
        step1_status_text = buf;
    } else if (is_step1_done_or_later) {
        step1_status_text = T_("FlowMeasureDoneApplied");
        for (int i = 0; i < sub_count; ++i) {
            if (!res.channels[i].connected) continue;
            const auto memo_key = make_sub_memo_key(i);
            const char* memo = s ? obs_data_get_string(s, memo_key.data()) : "";
            std::string name = (memo && *memo) ? memo : ("Ch." + std::to_string(i + 1));
            if (res.channels[i].measured) {
                char line[192];
                snprintf(line, sizeof(line), "\n  Ch.%d %s : %.1f ms",
                         i + 1, name.c_str(), d->sub[i].delay_ms);
                step1_status_text += line;
            } else {
                step1_status_text +=
                    "\n  Ch." + std::to_string(i + 1) + " " + name + " : " + T_("FlowChFailed");
            }
        }
    } else {
        step1_status_text = T_("FlowNotMeasured");
    }
    obs_properties_add_text(props, "flow_s1_status", step1_status_text.c_str(), OBS_TEXT_INFO);

    auto* retry_failed_btn = obs_properties_add_button2(
        props, "flow_retry_btn", T_("FlowRetryFailed"), cb_flow_retry_failed, d);
    obs_property_set_enabled(retry_failed_btn,
        is_step1_done && (res.measured_count < res.connected_count));

    if (s) obs_data_release(s);
}

} // namespace

bool try_get_parent_audio_sync_offset_ns(DelayStreamData* d, int64_t& out_offset_ns) {
    if (!d || !d->context) return false;

    obs_source_t* parent = obs_filter_get_parent(d->context);
    if (!parent) parent = obs_filter_get_target(d->context);
    if (!parent || plugin_main_obs_services::is_obs_source_removed(parent)) return false;

    out_offset_ns = obs_source_get_sync_offset(parent);
    return true;
}

void add_plugin_group(obs_properties_t* props, DelayStreamData* d) {
    if (!props || !d) return;
    obs_properties_t* grp = obs_properties_create();

    obs_property_t* about_p = obs_properties_add_text(grp, "about_info", "", OBS_TEXT_INFO);
    obs_property_set_long_description(about_p,
        "v" PLUGIN_VERSION " | (C) 2026 Mazzn1987, Chigiri Tsutsumi | GPL 2.0+<br>"
        "<a href=\"https://github.com/MZ1987Records/obs-delay-stream\">GitHub</a> | "
        "<a href=\"https://mz1987records.booth.pm/items/8134637\">Booth</a>");
    obs_property_text_set_info_word_wrap(about_p, false);

    if (d->is_duplicate_instance) {
        obs_properties_add_text(
            grp,
            "duplicate_instance_warning",
            "複数の obs-delay-stream フィルタを使用することはできません。",
            OBS_TEXT_INFO);
    } else {
        if ((obs_get_version() >> 24) < 32) {
            obs_property_t* ver_warn_p = obs_properties_add_text(
                grp,
                "obs_version_warning_top",
                T_("ObsVersionWarning"),
                OBS_TEXT_INFO);
            obs_property_text_set_info_word_wrap(ver_warn_p, true);
        }
        int64_t sync_offset_ns = 0;
        if (try_get_parent_audio_sync_offset_ns(d, sync_offset_ns) &&
            sync_offset_ns != REQUIRED_AUDIO_SYNC_OFFSET_NS) {
            obs_property_t* warn_p = obs_properties_add_text(
                grp,
                "audio_sync_offset_warning_top",
                T_("AudioSyncOffsetWarning"),
                OBS_TEXT_INFO);
            obs_property_text_set_info_word_wrap(warn_p, true);
        }
    }

    obs_properties_add_group(props, "grp_plugin", T_("Plugin"), OBS_GROUP_NORMAL, grp);
}

void add_stream_group(obs_properties_t* props, DelayStreamData* d) {
    if (!props || !d) return;
    obs_properties_t* grp = obs_properties_create();
    obs_property_t* sid_p =
        obs_properties_add_text(grp, "stream_id", T_("StreamId"), OBS_TEXT_DEFAULT);
    obs_property_set_modified_callback2(sid_p, cb_stream_id_changed, d);
    obs_property_set_enabled(sid_p, false);
    {
        char info[128];
        snprintf(info, sizeof(info), T_("AutoIpFmt"), d->auto_ip.c_str());
        obs_properties_add_text(grp, "auto_ip_info", info, OBS_TEXT_INFO);
    }
    obs_property_t* ip_p =
        obs_properties_add_text(grp, "host_ip_manual", T_("IpOverride"), OBS_TEXT_DEFAULT);
    obs_property_t* port_p =
        obs_properties_add_int(grp, "ws_port", T_("WsPort"), 1, 65535, 1);
    if (d->router_running.load()) {
        obs_property_set_enabled(sid_p, false);
        obs_property_set_enabled(ip_p, false);
        obs_property_set_enabled(port_p, false);
    }
    obs_properties_add_group(props, "grp_stream", T_("GroupStreamId"), OBS_GROUP_NORMAL, grp);
}

void add_ws_group(obs_properties_t* props, DelayStreamData* d, bool has_sid) {
    if (!props || !d) return;
    bool ws_running = d->router_running.load();
    int ws_port = ws_running
        ? (int)d->router.port()
        : d->ws_port.load(std::memory_order_relaxed);

    char ws_title_buf[96];
    std::string ws_title;
    if (ws_running) {
        snprintf(ws_title_buf, sizeof(ws_title_buf), T_("WsRunningFmt"), ws_port);
        ws_title = ws_title_buf;
    } else {
        ws_title = T_("WsStopped");
    }
    if (ws_running && !d->ws_send_enabled.load()) {
        ws_title += T_("WsPausedSuffix");
    }

    obs_properties_t* grp = obs_properties_create();
    obs_property_t* codec_p = obs_properties_add_list(
        grp, "audio_codec", T_("AudioCodec"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_set_modified_callback2(codec_p, cb_audio_codec_changed, d);
    obs_property_list_add_int(codec_p, "Opus", 0);
    obs_property_list_add_int(codec_p, T_("CodecPcm"), 1);

    obs_property_t* opus_bitrate_p = obs_properties_add_int(
        grp, "opus_bitrate_kbps", T_("OpusBitrateKbps"), 6, 510, 1);
    obs_property_t* opus_sample_rate_p = obs_properties_add_list(
        grp, "opus_sample_rate", T_("OpusSampleRate"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(opus_sample_rate_p, "8000", 8000);
    obs_property_list_add_int(opus_sample_rate_p, "12000", 12000);
    obs_property_list_add_int(opus_sample_rate_p, "16000", 16000);
    obs_property_list_add_int(opus_sample_rate_p, "24000", 24000);
    obs_property_list_add_int(opus_sample_rate_p, "48000", 48000);
    uint32_t input_sr = d->sample_rate > 0 ? d->sample_rate : 48000;
    char pcm_sr_info[128];
    snprintf(pcm_sr_info, sizeof(pcm_sr_info), T_("PcmInputSampleRateFmt"), input_sr);
    obs_property_t* pcm_sr_info_p = obs_properties_add_text(
        grp, "pcm_input_sample_rate_info", pcm_sr_info, OBS_TEXT_INFO);
    obs_property_text_set_info_word_wrap(pcm_sr_info_p, false);
    obs_property_t* quant_bits_p = obs_properties_add_list(
        grp, "quantization_bits", T_("QuantizationBits"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(quant_bits_p, "8", 8);
    obs_property_list_add_int(quant_bits_p, "16", 16);
    obs_property_t* mono_mix_p =
        obs_properties_add_bool(grp, "audio_mono", T_("AudioMono"));
    obs_property_t* pcm_ds_ratio_p = obs_properties_add_list(
        grp, "pcm_downsample_ratio", T_("PcmDownsampleRatio"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(pcm_ds_ratio_p, T_("PcmDownsampleRatioNone"), 1);
    obs_property_list_add_int(pcm_ds_ratio_p, "1/2", 2);
    obs_property_list_add_int(pcm_ds_ratio_p, "1/4", 4);
    obs_properties_add_stepper(
        grp, "playback_buffer_ms_stepper", T_("PlaybackBufferMs"),
        "playback_buffer_ms",
        PLAYBACK_BUFFER_MIN_MS, PLAYBACK_BUFFER_MAX_MS,
        PLAYBACK_BUFFER_DEFAULT_MS, 0, " ms", true);

    if (ws_running) {
        obs_property_set_enabled(codec_p, false);
        obs_property_set_enabled(opus_bitrate_p, false);
        obs_property_set_enabled(opus_sample_rate_p, false);
        obs_property_set_enabled(quant_bits_p, false);
        obs_property_set_enabled(mono_mix_p, false);
        obs_property_set_enabled(pcm_ds_ratio_p, false);
    }

    if (d->context) {
        obs_data_t* s = obs_source_get_settings(d->context);
        if (s) {
            plugin_main_config::apply_codec_option_visibility(grp, s);
            obs_data_release(s);
        }
    }

    const ObsColorButtonSpec ws_buttons[] = {
        {
            "ws_server_start_btn",
            T_("WsServerStartShort"),
            cb_ws_server_start,
            d,
            (!ws_running && has_sid),
            UI_COLOR_START_BUTTON_BG,
            UI_COLOR_BUTTON_TEXT,
        },
        {
            "ws_server_stop_btn",
            T_("WsServerStopShort"),
            cb_ws_server_stop,
            d,
            (ws_running),
            UI_COLOR_STOP_BUTTON_BG,
            UI_COLOR_BUTTON_TEXT,
        },
    };
    obs_properties_add_color_button_row(
        grp, "ws_server_controls_row", T_("WsServerControls"), ws_buttons,
        sizeof(ws_buttons) / sizeof(ws_buttons[0]));
    char ws_firewall_note[160];
    snprintf(ws_firewall_note, sizeof(ws_firewall_note),
        T_("WsFirewallNoteFmt"), ws_port);
    obs_property_t* fw_note_p = obs_properties_add_text(
        grp, "ws_firewall_note", ws_firewall_note, OBS_TEXT_INFO);
    obs_property_text_set_info_word_wrap(fw_note_p, false);

    obs_property_t* send_p = obs_properties_add_bool(grp, "ws_send_paused", T_("WsSendPause"));
    if (!ws_running) {
        obs_property_set_enabled(send_p, false);
    }
    obs_property_t* delay_p = obs_properties_add_bool(grp, "delay_disable", T_("DelayDisable"));
    if (!ws_running) {
        obs_property_set_enabled(delay_p, false);
    }
    obs_properties_add_group(props, "grp_ws", ws_title.c_str(), OBS_GROUP_NORMAL, grp);
}

void add_tunnel_group(obs_properties_t* props, DelayStreamData* d) {
    if (!props || !d) return;
    TunnelState ts = d->tunnel.state();
    std::string turl = d->tunnel.url();
    std::string terr = d->tunnel.error();
    const char* tunnel_title =
        (ts == TunnelState::Running)
            ? T_("TunnelRunning")
            : T_("TunnelStopped");

    obs_properties_t* grp = obs_properties_create();
    obs_properties_add_text(grp, "cloudflared_exe_path", T_("CloudflaredExePath"), OBS_TEXT_DEFAULT);

    bool show_tunnel_start_note = false;
    bool cloudflared_downloading =
        d->tunnel.cloudflared_downloading();
    bool ws_running = d->router_running.load();
    bool tunnel_running = (ts == TunnelState::Running);
    bool tunnel_busy = cloudflared_downloading || (ts == TunnelState::Starting);
    const ObsColorButtonSpec tunnel_buttons[] = {
        {
            "tunnel_start_btn",
            T_("TunnelStartShort"),
            cb_tunnel_start,
            d,
            (!tunnel_running && !tunnel_busy && ws_running),
            UI_COLOR_START_BUTTON_BG,
            UI_COLOR_BUTTON_TEXT,
        },
        {
            "tunnel_stop_btn",
            T_("TunnelStopShort"),
            cb_tunnel_stop,
            d,
            tunnel_running,
            UI_COLOR_STOP_BUTTON_BG,
            UI_COLOR_BUTTON_TEXT,
        },
    };
    obs_properties_add_color_button_row(
        grp, "tunnel_controls_row", T_("TunnelControls"), tunnel_buttons,
        sizeof(tunnel_buttons) / sizeof(tunnel_buttons[0]));

    if (cloudflared_downloading) {
        obs_properties_add_text(grp, "tunnel_state_info",
            T_("CloudflaredDownloading"), OBS_TEXT_INFO);
    } else if (ts == TunnelState::Starting) {
        obs_properties_add_text(grp, "tunnel_state_info",
            T_("TunnelStarting"), OBS_TEXT_INFO);
    }
    show_tunnel_start_note = (!ws_running && !tunnel_running && !tunnel_busy);

    std::string tunnel_domain = extract_host_from_url(turl);
    if (!tunnel_domain.empty()) {
        char db[320];
        snprintf(db, sizeof(db), T_("TunnelAssignedDomainFmt"), tunnel_domain.c_str());
        obs_properties_add_text(grp, "tunnel_domain_info", db, OBS_TEXT_INFO);
    } else {
        obs_properties_add_text(grp, "tunnel_domain_info",
            T_("TunnelUnassignedDomain"), OBS_TEXT_INFO);
    }

    if (show_tunnel_start_note) {
        obs_properties_add_text(grp, "tunnel_start_note",
            T_("TunnelStartNote"), OBS_TEXT_INFO);
    }

    if (ts == TunnelState::Running && !turl.empty()) {
        // URL 表示は「演者別チャンネル」に集約
    } else if (ts == TunnelState::Error && !terr.empty()) {
        char eb[256];
        snprintf(eb, sizeof(eb), T_("TunnelErrorFmt"), terr.c_str());
        obs_properties_add_text(grp, "tunnel_error", eb, OBS_TEXT_INFO);
    }
    obs_properties_add_group(props, "grp_tunnel", tunnel_title, OBS_GROUP_NORMAL, grp);
}

void add_flow_group(obs_properties_t* props, DelayStreamData* d) {
    if (!props || !d) return;
    obs_properties_t* grp = obs_properties_create();
    {
        obs_property_t* ping_count_p = obs_properties_add_list(
            grp, "ping_count", T_("PingCount"),
            OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
        obs_property_list_add_int(ping_count_p, "10", 10);
        obs_property_list_add_int(ping_count_p, "20", 20);
        obs_property_list_add_int(ping_count_p, "30", 30);
        obs_property_list_add_int(ping_count_p, "40", 40);
        obs_property_list_add_int(ping_count_p, "50", 50);
        if (d->flow.phase() != FlowPhase::Idle) {
            obs_property_set_enabled(ping_count_p, false);
        }
    }
    build_flow_panel(grp, d);
    if ((obs_get_version() >> 24) < 32) {
        obs_property_t* ver_warn_p = obs_properties_add_text(
            grp,
            "obs_version_warning",
            T_("ObsVersionWarning"),
            OBS_TEXT_INFO);
        obs_property_text_set_info_word_wrap(ver_warn_p, true);
    }
    int64_t sync_offset_ns = 0;
    if (try_get_parent_audio_sync_offset_ns(d, sync_offset_ns) &&
        sync_offset_ns != REQUIRED_AUDIO_SYNC_OFFSET_NS) {
        obs_property_t* warn_p = obs_properties_add_text(
            grp,
            "audio_sync_offset_warning",
            T_("AudioSyncOffsetWarning"),
            OBS_TEXT_INFO);
        obs_property_text_set_info_word_wrap(warn_p, true);
    }
    int active_channels = d->sub_ch_count;
    int connected_channels = 0;
    for (int i = 0; i < active_channels; ++i) {
        if (d->router.client_count(i) > 0) {
            ++connected_channels;
        }
    }
    char flow_title[192];
    snprintf(flow_title, sizeof(flow_title), T_("GroupSyncFlowWithConn"),
             T_("GroupSyncFlow"), connected_channels, active_channels);
    obs_properties_add_group(props, "grp_flow", flow_title, OBS_GROUP_NORMAL, grp);
}

void add_master_group(obs_properties_t* props, DelayStreamData* d) {
    if (!props || !d) return;
    obs_properties_t* grp = obs_properties_create();
    bool auto_mode = true;
    double master_delay_ms = 0.0;
    {
        obs_data_t* s = obs_source_get_settings(d->context);
        if (s) {
            auto_mode       = obs_data_get_bool(s, "rtmp_url_auto");
            master_delay_ms = obs_data_get_double(s, "master_delay_ms");
            obs_data_release(s);
        }
    }
    obs_property_t* auto_p = obs_properties_add_bool(grp, "rtmp_url_auto", T_("RtmpUrlAuto"));
    obs_property_set_modified_callback2(auto_p, cb_rtmp_url_auto_changed, d);
    obs_property_t* url_p =
        obs_properties_add_text(grp, "rtmp_url", T_("RtmpUrl"), OBS_TEXT_DEFAULT);
    obs_property_set_enabled(url_p, !auto_mode);
    add_flow_rtmp_measure_section(grp, d);
    char master_delay_text[128];
    snprintf(master_delay_text, sizeof(master_delay_text), T_("MasterDelayFmt"), master_delay_ms);
    obs_properties_add_text(grp, "master_delay_display", master_delay_text, OBS_TEXT_INFO);
    obs_properties_add_group(props, "grp_master", T_("GroupMasterRtmp"), OBS_GROUP_NORMAL, grp);
}

} // namespace plugin_main_properties_ui
