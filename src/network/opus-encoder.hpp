#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <obs-module.h>
#include <string>
#include <vector>

#include <media-io/audio-resampler.h>
#include <opus.h>

#include "network/stream-router-utils.hpp"

namespace ods::network {

	using OpusPacketList = std::vector<std::shared_ptr<std::string>>; ///< Opus エンコード済みパケット列

	/**
	 * チャンネル単位の Opus エンコード状態。
	 *
	 * libopus で直接エンコードし、サンプルレート変換は libobs の `audio_resampler`
	 * （obs.dll エクスポート）を用いる。FFmpeg（libav* / libsw*）には依存しないため、
	 * OBS が同梱する FFmpeg の世代に左右されずロードできる。
	 */
	struct OpusEncoder {
		bool disabled           = false; ///< 初期化失敗などで無効化されているか
		bool flush_pending      = false; ///< ディレイ変更に伴うフラッシュ保留フラグ
		int  input_sample_rate  = 0;     ///< 入力サンプルレート
		int  output_sample_rate = 0;     ///< 出力（Opus）サンプルレート
		int  channels           = 0;     ///< チャンネル数
		int  bitrate_kbps       = 0;     ///< 目標ビットレート (kbps)
		int  complexity         = 10;    ///< Opus complexity

		bool ready() const { return enc_ != nullptr; } ///< エンコーダが初期化済みか

		// 内部リソースを解放して初期状態に戻す。
		void reset();

		// エンコーダを初期化する。
		bool init(int input_sample_rate,
				  int num_channels,
				  int bitrate_kbps,
				  int target_sample_rate);

		// 入力 PCM（インターリーブ float）を取り込み、揃った完全フレームを
		// すべてエンコードして `out` に追加する。端数サンプルは次回まで保持する。
		bool feed(const float *data, size_t frames, uint32_t magic_opus, OpusPacketList &out);

		// 保持中の完全フレームを吐き切ったのち、端数を破棄してエンコーダの
		// 予測状態をリセットする。ディレイ変更時のフラッシュ用。
		bool flush(uint32_t magic_opus, OpusPacketList &out);

	private:

		::OpusEncoder     *enc_       = nullptr; ///< libopus エンコーダ
		audio_resampler_t *resampler_ = nullptr; ///< 入力≠出力レート時のみ生成
		std::vector<float> fifo_;                ///< インターリーブ float @ output rate
		size_t             fifo_head_ = 0;       ///< fifo_ の読み出し開始位置（float 要素単位）
		int                frame_size = 0;       ///< Opus 1 フレーム当たりサンプル数（20ms）

		// fifo_ 内の完全フレームをすべてエンコードして out に追加する。
		bool encode_ready_frames(uint32_t magic_opus, OpusPacketList &out);

		// 消費済み先頭領域を切り詰める。
		void compact_fifo();
	};

} // namespace ods::network
