/*
 * opus-encoder.cpp
 *
 * libopus による Opus エンコード実装。サンプルレート変換は libobs の
 * audio_resampler（obs.dll 内）に委譲し、FFmpeg(libav*) には依存しない。
 * これによりプラグインの import から avcodec/avutil/swresample が消え、
 * OBS 同梱 FFmpeg の世代差に影響されずロードできる。
 */

#include "network/opus-encoder.hpp"

#include <cstring>

namespace ods::network {

	namespace {

		// チャンネル数に対応する libobs スピーカーレイアウト。
		// リサンプルではレートのみ変換し、チャンネル数は変えない。
		speaker_layout speakers_from_channels(int ch) {
			switch (ch) {
			case 1:
				return SPEAKERS_MONO;
			case 2:
				return SPEAKERS_STEREO;
			case 3:
				return SPEAKERS_2POINT1;
			case 4:
				return SPEAKERS_4POINT0;
			case 5:
				return SPEAKERS_4POINT1;
			case 6:
				return SPEAKERS_5POINT1;
			case 8:
				return SPEAKERS_7POINT1;
			default:
				return SPEAKERS_STEREO;
			}
		}

	} // namespace

	void OpusEncoder::reset() {
		if (enc_) {
			opus_encoder_destroy(enc_);
			enc_ = nullptr;
		}
		if (resampler_) {
			audio_resampler_destroy(resampler_);
			resampler_ = nullptr;
		}
		fifo_.clear();
		fifo_head_         = 0;
		frame_size         = 0;
		input_sample_rate  = 0;
		output_sample_rate = 0;
		channels           = 0;
		bitrate_kbps       = 0;
		complexity         = 10;
		disabled           = false;
		flush_pending      = false;
	}

	bool OpusEncoder::init(int in_rate, int num_channels, int br_kbps, int target_sample_rate) {
		reset();

		int rate = is_valid_opus_sample_rate(target_sample_rate) ? target_sample_rate : 48000;
		if (br_kbps < 24) br_kbps = 24;
		if (br_kbps > 320) br_kbps = 320;

		int err = OPUS_OK;
		enc_    = opus_encoder_create(rate, num_channels, OPUS_APPLICATION_AUDIO, &err);
		if (err != OPUS_OK || !enc_) {
			blog(LOG_WARNING, "[obs-delay-stream] Opus encoder create failed (%d)", err);
			enc_     = nullptr;
			disabled = true;
			return false;
		}
		opus_encoder_ctl(enc_, OPUS_SET_BITRATE(br_kbps * 1000));
		opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(complexity));
		opus_encoder_ctl(enc_, OPUS_SET_VBR(1));

		frame_size = rate / 50; // 20ms

		// 入力レートが Opus 出力レートと異なる場合のみリサンプラを生成する。
		// libopus はインターリーブ float をそのまま受け取れるため、
		// AUDIO_FORMAT_FLOAT（インターリーブ）で揃える。
		if (in_rate != rate) {
			resample_info src{};
			src.samples_per_sec = static_cast<uint32_t>(in_rate);
			src.format          = AUDIO_FORMAT_FLOAT;
			src.speakers        = speakers_from_channels(num_channels);
			resample_info dst   = src;
			dst.samples_per_sec = static_cast<uint32_t>(rate);
			resampler_          = audio_resampler_create(&dst, &src);
			if (!resampler_) {
				blog(LOG_WARNING, "[obs-delay-stream] audio_resampler_create failed");
				disabled = true;
				return false;
			}
		}

		input_sample_rate  = in_rate;
		output_sample_rate = rate;
		channels           = num_channels;
		bitrate_kbps       = br_kbps;
		fifo_.clear();
		fifo_head_ = 0;
		return true;
	}

	bool OpusEncoder::feed(const float *data, size_t frames, uint32_t magic_opus, OpusPacketList &out) {
		if (!enc_) return true;

		if (data && frames > 0) {
			if (!resampler_) {
				fifo_.insert(fifo_.end(), data, data + frames * static_cast<size_t>(channels));
			} else {
				uint8_t       *out_planes[MAX_AV_PLANES] = {};
				uint32_t       out_frames                = 0;
				uint64_t       ts_offset                 = 0;
				const uint8_t *in_planes[MAX_AV_PLANES]  = {};
				in_planes[0]                             = reinterpret_cast<const uint8_t *>(data);
				if (!audio_resampler_resample(
						resampler_,
						out_planes,
						&out_frames,
						&ts_offset,
						in_planes,
						static_cast<uint32_t>(frames))) {
					return false;
				}
				if (out_frames > 0 && out_planes[0]) {
					const float *res = reinterpret_cast<const float *>(out_planes[0]);
					fifo_.insert(fifo_.end(), res, res + static_cast<size_t>(out_frames) * channels);
				}
			}
		}

		return encode_ready_frames(magic_opus, out);
	}

	bool OpusEncoder::encode_ready_frames(uint32_t magic_opus, OpusPacketList &out) {
		if (!enc_ || frame_size <= 0 || channels <= 0) return true;

		const size_t  need = static_cast<size_t>(frame_size) * static_cast<size_t>(channels);
		unsigned char packet[4000]; // Opus 推奨の最大パケットサイズ

		while (fifo_.size() - fifo_head_ >= need) {
			const float *pcm = fifo_.data() + fifo_head_;
			int          n   = opus_encode_float(
				enc_,
				pcm,
				frame_size,
				packet,
				static_cast<opus_int32>(sizeof(packet)));
			if (n < 0) {
				blog(LOG_WARNING, "[obs-delay-stream] opus_encode_float failed (%d)", n);
				return false;
			}
			if (n > 0) {
				auto      p   = std::make_shared<std::string>(16 + static_cast<size_t>(n), '\0');
				uint32_t *hdr = reinterpret_cast<uint32_t *>(&(*p)[0]);
				hdr[0]        = magic_opus;
				hdr[1]        = static_cast<uint32_t>(output_sample_rate);
				hdr[2]        = static_cast<uint32_t>(channels);
				hdr[3]        = static_cast<uint32_t>(frame_size);
				std::memcpy(&(*p)[16], packet, static_cast<size_t>(n));
				out.push_back(std::move(p));
			}
			fifo_head_ += need;
		}

		compact_fifo();
		return true;
	}

	void OpusEncoder::compact_fifo() {
		if (fifo_head_ == 0) return;
		if (fifo_head_ >= fifo_.size()) {
			fifo_.clear();
		} else {
			fifo_.erase(fifo_.begin(), fifo_.begin() + static_cast<std::ptrdiff_t>(fifo_head_));
		}
		fifo_head_ = 0;
	}

	bool OpusEncoder::flush(uint32_t magic_opus, OpusPacketList &out) {
		if (!enc_) return true;

		// 端数（部分フレーム）は破棄せず保持し、続く feed() の新規サンプルと
		// 連続して符号化する。破棄すると再同期ごとに最大 1 フレーム(20ms)の
		// タイムライン欠損が生じ、受信側のアンダーラン（再同期ギャップ）を誘発する。
		// ディレイ変更前後の不連続は 1 フレーム内の軽微なクリックで済み、Opus は
		// これをそのまま符号化できる。エンコーダ状態もリセットしない（連続デコーダ整合のため）。
		return encode_ready_frames(magic_opus, out);
	}

} // namespace ods::network
