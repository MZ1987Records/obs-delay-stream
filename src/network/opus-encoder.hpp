#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <obs-module.h>
#include <string>
#include <vector>

#ifdef __cplusplus
extern "C" {
#endif
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#ifdef __cplusplus
}
#endif

#include "network/stream-router-utils.hpp"

namespace ods::network {

	using OpusPacketList = std::vector<std::shared_ptr<std::string>>; ///< Opus エンコード済みパケット列

	/**
	 * チャンネル単位の Opus エンコード状態。
	 */
	struct OpusEncoder {
		bool    disabled           = false; ///< 初期化失敗などで無効化されているか
		bool    flush_pending      = false; ///< 遅延変更に伴うフラッシュ保留フラグ
		int     input_sample_rate  = 0;     ///< 入力サンプルレート
		int     output_sample_rate = 0;     ///< 出力サンプルレート
		int     channels           = 0;     ///< チャンネル数
		int     frame_size         = 0;     ///< Opus 1 フレーム当たりサンプル数
		int     bitrate_kbps       = 0;     ///< 目標ビットレート (kbps)
		int     complexity         = 10;    ///< Opus complexity
		int64_t pts                = 0;     ///< 連続 PTS

		AVCodecContext *ctx   = nullptr; ///< FFmpeg コーデックコンテキスト
		SwrContext     *swr   = nullptr; ///< リサンプラ
		AVAudioFifo    *fifo  = nullptr; ///< フレーム境界調整用 FIFO
		AVFrame        *frame = nullptr; ///< エンコード入力フレーム
		AVPacket       *pkt   = nullptr; ///< エンコード出力パケット

		// 内部リソースを解放して初期状態に戻す。
		void reset();

		// エンコーダを初期化する。
		bool init(int input_sample_rate,
				  int num_channels,
				  int bitrate_kbps,
				  int target_sample_rate);

		// FIFO 内の完全フレームをすべてエンコードして `out` に追加する。
		// 端数サンプルは破棄し、エンコーダ予測状態をリセットする。
		// PTS は継続する。
		bool drain(uint32_t magic_opus, OpusPacketList &out);

		// 入力 PCM を FIFO に投入する。
		bool feed_fifo(const float *data, size_t frames);
	};

} // namespace ods::network
