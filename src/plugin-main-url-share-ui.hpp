#pragma once

#include <obs-module.h>

struct DelayStreamData;

namespace plugin_main_url_share_ui {

void add_url_share_group(obs_properties_t* props, DelayStreamData* d);

} // namespace plugin_main_url_share_ui
