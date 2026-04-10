#pragma once

#include <cstdint>
#include <obs-module.h>

namespace ods::plugin {
	struct DelayStreamData;
}

namespace ods::audio {

	void ensure_audio_processing_initialized(ods::plugin::DelayStreamData *d, uint32_t sample_rate, uint32_t num_channels);
	obs_audio_data *filter_audio_delay_stream(ods::plugin::DelayStreamData *d, obs_audio_data *audio);

} // namespace ods::audio
