#pragma once

#include <obs-module.h>

struct DelayStreamData;

namespace plugin_main_update {

void apply_settings(DelayStreamData* d, obs_data_t* settings);

} // namespace plugin_main_update
