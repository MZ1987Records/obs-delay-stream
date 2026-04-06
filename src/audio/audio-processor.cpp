#include "audio/audio-processor.hpp"

#include "plugin/plugin-settings.hpp"
#include "plugin/plugin-state.hpp"

namespace plugin_main_audio_processing {

namespace {

static float calc_effective_sub_delay_value_ms(const DelayStreamData *d,
											   float                  base_delay_ms,
											   float                  adjust_ms) {
	if (!d) return 0.0f;
	return plugin_settings::calc_effective_sub_delay_value_ms(
		base_delay_ms,
		adjust_ms,
		d->sub_offset_ms);
}

static uint32_t calc_effective_sub_delay_ms(const DelayStreamData *d,
											float                  base_delay_ms,
											float                  adjust_ms) {
	if (!d || !d->enabled.load(std::memory_order_relaxed)) return 0;
	return static_cast<uint32_t>(
		calc_effective_sub_delay_value_ms(d, base_delay_ms, adjust_ms));
}

} // namespace

void apply_sub_delay_to_buffer(DelayStreamData *d, int ch) {
	if (!d || ch < 0 || ch >= MAX_SUB_CH) return;
	d->sub[ch].buf.set_delay_ms(
		calc_effective_sub_delay_ms(d, d->sub[ch].delay_ms, d->sub[ch].adjust_ms));
}

void ensure_audio_processing_initialized(DelayStreamData *d, uint32_t sr, uint32_t ch) {
	if (!d) return;
	if (d->initialized && d->sample_rate == sr && d->channels == ch) return;
	d->sample_rate = sr;
	d->channels    = ch;
	d->master_buf.init(sr, ch);
	d->master_buf.set_delay_ms((uint32_t)d->master_delay_ms);
	for (int i = 0; i < MAX_SUB_CH; ++i) {
		d->sub[i].buf.init(sr, ch);
		apply_sub_delay_to_buffer(d, i);
	}
	d->work_buf.resize(65536 * ch, 0.0f);
	d->initialized = true;
}

obs_audio_data *filter_audio_delay_stream(DelayStreamData *d, obs_audio_data *audio) {
	if (!d || !audio) return audio;
	if (d->is_duplicate_instance) return audio;
	if (audio->frames == 0) return audio;

	const audio_output_info *info = audio_output_get_info(obs_get_audio());
	uint32_t                 sr   = info ? info->samples_per_sec : 48000;
	uint32_t                 ch   = (uint32_t)audio_output_get_channels(obs_get_audio());
	if (ch < 1) ch = 2;
	ensure_audio_processing_initialized(d, sr, ch);

	size_t frames = audio->frames;
	size_t total  = frames * ch;
	if (d->work_buf.size() < total * 3) d->work_buf.resize(total * 3, 0.0f);
	float *in  = d->work_buf.data();
	float *out = d->work_buf.data() + total;
	float *sub = d->work_buf.data() + total * 2;
	for (uint32_t c = 0; c < ch; ++c) {
		if (!audio->data[c]) continue;
		const float *src = reinterpret_cast<const float *>(audio->data[c]);
		for (size_t f = 0; f < frames; ++f)
			in[f * ch + c] = src[f];
	}
	bool en        = d->enabled.load(std::memory_order_relaxed);
	bool ws        = d->ws_send_enabled.load(std::memory_order_relaxed);
	bool rr        = d->router_running.load(std::memory_order_relaxed);
	bool has_sid   = !d->get_stream_id().empty();
	int  sub_count = d->sub_ch_count;

	if (en) {
		d->master_buf.process(in, out, frames);
		for (uint32_t c = 0; c < ch; ++c) {
			if (!audio->data[c]) continue;
			float *dst = reinterpret_cast<float *>(audio->data[c]);
			for (size_t f = 0; f < frames; ++f)
				dst[f] = out[f * ch + c];
		}
		if (ws && rr && has_sid) {
			for (int i = 0; i < sub_count; ++i) {
				d->sub[i].buf.process(in, sub, frames);
				d->router.send_audio(i, sub, frames, sr, ch);
			}
		}
	} else {
		if (ws && rr && has_sid) {
			for (int i = 0; i < sub_count; ++i) {
				d->router.send_audio(i, in, frames, sr, ch);
			}
		}
	}
	return audio;
}

} // namespace plugin_main_audio_processing
