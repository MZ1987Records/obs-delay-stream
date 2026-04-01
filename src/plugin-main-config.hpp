#pragma once

#include <string>

#include <obs-module.h>

namespace plugin_main_config {

std::string make_default_sub_memo(int counter);
void apply_codec_option_visibility(obs_properties_t* props, obs_data_t* settings);
void set_delay_stream_defaults(obs_data_t* settings);

} // namespace plugin_main_config
