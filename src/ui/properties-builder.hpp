#pragma once

#include <cstdint>

#include <obs-module.h>

struct DelayStreamData;

namespace plugin_main_properties_ui {

bool try_get_parent_audio_sync_offset_ns(DelayStreamData* d, int64_t& out_offset_ns);

class PropertiesBuilder {
public:
    PropertiesBuilder(obs_properties_t* props, DelayStreamData* d);

    // --- properties-builder.cpp が実装 ---
    void add_plugin_group();
    void add_stream_group();
    void add_ws_group(bool has_sid);
    void add_tunnel_group();
    void add_flow_group();
    void add_master_group();

    // --- properties-channels.cpp が実装 ---
    void add_sub_channels_group();

    // --- properties-delay.cpp が実装 ---
    void add_sub_offset_group();
    void add_delay_summary_group();

    // --- properties-url-share.cpp が実装 ---
    void add_url_share_group();

private:
    obs_properties_t* props_;
    DelayStreamData*  d_;

    // properties-ui 内部ヘルパー
    void add_flow_rtmp_measure_section(obs_properties_t* grp);
    void build_flow_panel(obs_properties_t* grp);

    // OBS コールバック (static = C 関数ポインタ互換)
    // properties-ui 由来
    static bool cb_rtmp_url_auto_changed(void*, obs_properties_t*, obs_property_t*, obs_data_t*);
    static bool cb_ws_server_start(obs_properties_t*, obs_property_t*, void*);
    static bool cb_ws_server_stop(obs_properties_t*, obs_property_t*, void*);
    static bool cb_tunnel_start(obs_properties_t*, obs_property_t*, void*);
    static bool cb_tunnel_stop(obs_properties_t*, obs_property_t*, void*);
    static bool cb_flow_start(obs_properties_t*, obs_property_t*, void*);
    static bool cb_flow_retry_failed(obs_properties_t*, obs_property_t*, void*);
    static bool cb_flow_start_step3(obs_properties_t*, obs_property_t*, void*);
    static bool cb_flow_reset(obs_properties_t*, obs_property_t*, void*);
    static bool cb_stream_id_changed(void*, obs_properties_t*, obs_property_t*, obs_data_t*);
    static bool cb_audio_codec_changed(void*, obs_properties_t*, obs_property_t*, obs_data_t*);
    // sub-channels-ui 由来
    static bool cb_sub_add(obs_properties_t*, obs_property_t*, void*);
    static bool cb_sub_remove(obs_properties_t*, obs_property_t*, void*);
    // url-share-ui 由来
    static bool cb_sub_copy_all(obs_properties_t*, obs_property_t*, void*);
};

} // namespace plugin_main_properties_ui
