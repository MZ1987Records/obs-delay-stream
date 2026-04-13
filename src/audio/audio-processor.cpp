#include "audio/audio-processor.hpp"

#include "plugin/plugin-state.hpp"
#include "plugin/plugin-settings.hpp"

#include <algorithm>

namespace ods::audio {

	using namespace ods::core;

	void ensure_audio_processing_initialized(ods::plugin::DelayStreamData *d,
											 uint32_t                      sample_rate,
											 uint32_t                      num_channels) {
		if (!d) return;
		if (d->initialized &&
			d->sample_rate == sample_rate &&
			d->channels == num_channels)
			return;
		d->sample_rate = sample_rate;
		d->channels    = num_channels;
		d->master_buf.init(sample_rate, num_channels);
		for (int i = 0; i < MAX_SUB_CH; ++i) {
			d->sub_channels[i].buf.init(sample_rate, num_channels);
		}
		ods::plugin::recalc_all_delays(d);
		d->work_buf.resize(65536 * num_channels, 0.0f);
		d->initialized = true;
	}

	obs_audio_data *filter_audio_delay_stream(ods::plugin::DelayStreamData *d, obs_audio_data *audio) {
		if (!d || !audio) return audio;
		if (d->is_warning_only_instance()) return audio;
		if (audio->frames == 0) return audio;

		const audio_output_info *info         = audio_output_get_info(obs_get_audio());
		uint32_t                 sample_rate  = info ? info->samples_per_sec : 48000;
		uint32_t                 num_channels = (uint32_t)audio_output_get_channels(obs_get_audio());
		if (num_channels < 1) num_channels = 2;
		ensure_audio_processing_initialized(d, sample_rate, num_channels);

		size_t frames = audio->frames;
		size_t total  = frames * num_channels;
		if (d->work_buf.size() < total * 3) d->work_buf.resize(total * 3, 0.0f);
		float *in      = d->work_buf.data();
		float *out     = d->work_buf.data() + total;
		float *sub_buf = d->work_buf.data() + total * 2;
		for (uint32_t c = 0; c < num_channels; ++c) {
			if (!audio->data[c]) continue;
			const float *src = reinterpret_cast<const float *>(audio->data[c]);
			for (size_t f = 0; f < frames; ++f)
				in[f * num_channels + c] = src[f];
		}
		bool is_enabled      = d->enabled.load(std::memory_order_relaxed);
		bool ws_send_enabled = d->ws_send_enabled.load(std::memory_order_relaxed);
		bool router_running  = d->router_running.load(std::memory_order_relaxed);
		bool has_sid         = !d->get_stream_id().empty();
		int  sub_count       = d->delay.sub_ch_count;

		if (is_enabled) {
			d->master_buf.process(in, out, frames);
			for (uint32_t c = 0; c < num_channels; ++c) {
				if (!audio->data[c]) continue;
				float *dst = reinterpret_cast<float *>(audio->data[c]);
				for (size_t f = 0; f < frames; ++f)
					dst[f] = out[f * num_channels + c];
			}

			// ミュートモード計測中は出力音声をゼロクリアする。
			const bool mute_active = d->probe_mute_active.load(std::memory_order_acquire);
			if (mute_active) {
				for (uint32_t c = 0; c < num_channels; ++c) {
					if (!audio->data[c]) continue;
					float *dst = reinterpret_cast<float *>(audio->data[c]);
					std::fill(dst, dst + frames, 0.0f);
				}
			}

			// RTSP E2E 計測用チャープ信号は 1 回だけ加算注入する。
			if (d->inject_impulse.exchange(false, std::memory_order_acq_rel)) {
				const float scale    = mute_active
										   ? ProbeSignal::kScaleMuted
										   : ProbeSignal::kScaleMix;
				const auto &chirp    = d->probe_signal.waveform();
				const int   inject_n = std::min<int>(static_cast<int>(chirp.size()),
													 static_cast<int>(frames));
				for (uint32_t c = 0; c < num_channels; ++c) {
					if (!audio->data[c]) continue;
					float *dst = reinterpret_cast<float *>(audio->data[c]);
					for (int i = 0; i < inject_n; ++i)
						dst[i] += chirp[static_cast<size_t>(i)] * scale;
				}
			}

			if (ws_send_enabled && router_running && has_sid) {
				for (int i = 0; i < sub_count; ++i) {
					d->sub_channels[i].buf.process(in, sub_buf, frames);
					d->router.send_audio(i, sub_buf, frames, sample_rate, num_channels);
				}
			}
		} else {
			if (ws_send_enabled && router_running && has_sid) {
				for (int i = 0; i < sub_count; ++i) {
					d->router.send_audio(i, in, frames, sample_rate, num_channels);
				}
			}
		}
		return audio;
	}

} // namespace ods::audio
