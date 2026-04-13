#pragma once

/**
 * チャープ信号によるプローブ生成と相関検出。
 *
 * 送信側: プリコンピュートした float32 チャープ波形を OBS 音声バッファへ加算注入する。
 * 受信側: int16 PCM ストリームに対して正規化相互相関を計算し、
 *         放物線補間でサブサンプル精度のチャープ到達時刻を特定する。
 */

#include <cstdint>
#include <vector>

namespace ods::audio {

	/**
	 * Hann 窓付きリニアチャープ波形の生成器。
	 *
	 * コンストラクタで kFreqStart→kFreqEnd のチャープ波形を生成し、
	 * 注入用（float32）と相関検出用（L2 正規化）の 2 形式で保持する。
	 * 生成後は不変であり、スレッド間で安全に共有できる。
	 */
	class ProbeSignal {
	public:

		static constexpr int    kSampleRate = 48000;  ///< 想定サンプルレート (Hz)
		static constexpr int    kLength     = 1024;   ///< チャープ長 (sample, 21.3 ms)
		static constexpr float  kAmplitude  = 0.04f;  ///< 基準振幅（加算合成, -28 dBFS）
		static constexpr float  kScaleMix   = 2.5f;   ///< ミックスモード注入スケール（実効 -20 dBFS）
		static constexpr float  kScaleMuted = 0.25f;  ///< ミュートモード注入スケール（実効 -40 dBFS）
		static constexpr double kFreqStart  = 1000.0; ///< 開始周波数 (Hz)
		static constexpr double kFreqEnd    = 3000.0; ///< 終了周波数 (Hz)

		ProbeSignal();

		const std::vector<float> &waveform() const;  ///< float32 波形（注入用）
		const std::vector<float> &reference() const; ///< L2 正規化リファレンス（相関用）
		int length() const;                          ///< サンプル数を返す

	private:

		std::vector<float> waveform_;  ///< Hann 窓付きリニアチャープ波形
		std::vector<float> reference_; ///< L2 ノルム = 1.0 に正規化
	};

	/**
	 * 正規化相互相関によるチャープ検出器。
	 *
	 * int16 PCM チャンクを逐次供給し、プローブチャープとの
	 * 正規化相互相関がしきい値を超えた時点で検出を報告する。
	 * チャンク境界をまたぐチャープも内部テールバッファで処理する。
	 */
	class ProbeDetector {
	public:

		static constexpr float kDetectThreshold = 0.20f; ///< 正規化相関の検出閾値

		struct DetectResult {
			bool   detected    = false; ///< チャープ検出成功フラグ
			double peak_offset = 0.0;   ///< 現在チャンク先頭基準のピーク位置 (sample, 放物線補間済み)
		};

		/// @param signal チャープ信号リファレンス（ProbeSignal から取得）
		explicit ProbeDetector(const ProbeSignal &signal);

		/// int16 PCM チャンクを供給して相関検出する。
		/// @param samples int16 PCM サンプル列
		/// @param count   サンプル数
		/// @return 検出結果。detected==true の場合 peak_offset がピーク位置。
		DetectResult feed(const int16_t *samples, size_t count);

		/// 内部状態をリセットする（新しい計測サイクルの開始時に呼ぶ）。
		void reset();

	private:

		const std::vector<float> &ref_;              ///< ProbeSignal::reference() への参照
		int                       ref_len_;          ///< リファレンス長
		std::vector<float>        tail_;             ///< 前チャンクの末尾 ref_len_-1 サンプル（float 変換済み）
		bool                      has_tail_ = false; ///< tail_ が有効データを持つか
	};

} // namespace ods::audio
