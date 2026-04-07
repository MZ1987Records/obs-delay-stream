#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ods::core {

	/**
	 * サンプルレート・チャンネル数に対応したリングバッファ遅延器。
	 *
	 * `set_delay_ms()` は任意スレッドから呼び出し可能で、
	 * `process()` は音声スレッドで呼び出す前提。
	 * `delay_samples_` を atomic で保持してクロススレッド更新に対応する。
	 */
	class DelayBuffer {
	public:

		/// バッファを初期化する。
		/// @param max_delay_ms 確保する最大遅延量 (ms)
		void init(uint32_t sample_rate, uint32_t channels, uint32_t max_delay_ms = 10000) {
			sample_rate_       = sample_rate;
			channels_          = channels;
			size_t max_samples = (size_t)sample_rate * max_delay_ms / 1000 + 1;
			buf_.assign(max_samples * channels, 0.0f);
			write_pos_ = 0;
			set_delay_ms(0);
		}

		/// 遅延量を設定する。任意スレッドから呼び出し可能。
		void set_delay_ms(uint32_t ms) {
			delay_ms_ = ms;
			size_t ds = (size_t)sample_rate_ * ms / 1000;
			// バッファ長を超えると読み出し位置が壊れるため上限を設ける。
			size_t max_samples = buf_.size() / std::max(channels_, 1u);
			if (ds > max_samples) ds = max_samples;
			delay_samples_.store(ds, std::memory_order_relaxed);
		}

		uint32_t get_delay_ms() const { return delay_ms_; }

		/// 音声を遅延処理する。`input` / `output` は interleaved float32。
		void process(const float *input, float *output, size_t frames) {
			if (buf_.empty() || channels_ == 0) {
				if (input != output)
					std::memcpy(output, input, frames * channels_ * sizeof(float));
				return;
			}
			size_t buf_frames = buf_.size() / channels_;
			// フレーム処理中に値が変わっても出力が揺れないように先に取得する。
			size_t ds = delay_samples_.load(std::memory_order_relaxed);

			for (size_t i = 0; i < frames; ++i) {
				for (uint32_t c = 0; c < channels_; ++c)
					buf_[write_pos_ * channels_ + c] = input[i * channels_ + c];

				size_t read_pos = (write_pos_ + buf_frames - ds) % buf_frames;
				for (uint32_t c = 0; c < channels_; ++c)
					output[i * channels_ + c] = buf_[read_pos * channels_ + c];

				write_pos_ = (write_pos_ + 1) % buf_frames;
			}
		}

		DelayBuffer() = default;

	private:

		uint32_t            sample_rate_ = 48000; ///< サンプルレート (Hz)
		uint32_t            channels_    = 2;     ///< チャンネル数
		uint32_t            delay_ms_    = 0;     ///< 現在の遅延量 (ms)
		std::atomic<size_t> delay_samples_{0};    ///< 遅延量をサンプル数に換算した値（atomic でクロススレッド更新）
		size_t              write_pos_ = 0;       ///< リングバッファの書き込み位置（フレーム単位）
		std::vector<float>  buf_;                 ///< interleaved float32 リングバッファ
	};

} // namespace ods::core
