#pragma once

#include <obs-module.h>

struct DelayStreamData;

namespace plugin_main_sub_channels_ui {

void add_sub_channels_group(obs_properties_t* props, DelayStreamData* d);

} // namespace plugin_main_sub_channels_ui
