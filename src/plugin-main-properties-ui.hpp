#pragma once

#include <cstdint>

#include <obs-module.h>

struct DelayStreamData;

namespace plugin_main_properties_ui {

bool try_get_parent_audio_sync_offset_ns(DelayStreamData* d, int64_t& out_offset_ns);
void add_plugin_group(obs_properties_t* props, DelayStreamData* d);
void add_stream_group(obs_properties_t* props, DelayStreamData* d);
void add_ws_group(obs_properties_t* props, DelayStreamData* d, bool has_sid);
void add_tunnel_group(obs_properties_t* props, DelayStreamData* d);
void add_flow_group(obs_properties_t* props, DelayStreamData* d);
void add_master_group(obs_properties_t* props, DelayStreamData* d);

} // namespace plugin_main_properties_ui
