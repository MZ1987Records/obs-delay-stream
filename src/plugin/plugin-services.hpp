#pragma once

#include <obs-module.h>
#include <string>

namespace ods::plugin {

bool        is_obs_source_removed(obs_source_t *source);
std::string get_obs_stream_url();
void        maybe_autofill_rtmp_url(obs_data_t *settings, bool force_refresh);
void        maybe_autofill_rtmp_url_from_source(obs_source_t *source, bool force_refresh);

} // namespace ods::plugin
