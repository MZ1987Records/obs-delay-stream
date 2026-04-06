#pragma once

#include <obs-module.h>
#include <string>

namespace ods::plugin {

std::string make_default_sub_memo(int counter);
void        apply_codec_option_visibility(obs_properties_t *props, obs_data_t *settings);
void        set_delay_stream_defaults(obs_data_t *settings);

} // namespace ods::plugin
