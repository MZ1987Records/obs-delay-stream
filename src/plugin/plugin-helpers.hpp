#pragma once

#include <obs-module.h>
#include <string>

namespace plugin_main_settings_helpers {

std::string resolve_rtmp_url_from_source(obs_source_t *source);
void        maybe_fill_cloudflared_path_from_auto(obs_source_t *source);
void        maybe_persist_cloudflared_path_after_auto_ready(obs_source_t *source);

} // namespace plugin_main_settings_helpers
