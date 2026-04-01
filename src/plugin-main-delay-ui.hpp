#pragma once

#include <obs-module.h>

struct DelayStreamData;

namespace plugin_main_delay_ui {

void add_sub_offset_group(obs_properties_t* props, DelayStreamData* d);
void add_delay_summary_group(obs_properties_t* props, DelayStreamData* d);

} // namespace plugin_main_delay_ui
