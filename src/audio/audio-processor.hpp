#pragma once

#include <cstdint>
#include <obs-module.h>

struct DelayStreamData;

namespace plugin_main_audio_processing {

void apply_sub_delay_to_buffer(DelayStreamData* d, int ch);
void ensure_audio_processing_initialized(DelayStreamData* d, uint32_t sr, uint32_t ch);
obs_audio_data* filter_audio_delay_stream(DelayStreamData* d, obs_audio_data* audio);

} // namespace plugin_main_audio_processing
