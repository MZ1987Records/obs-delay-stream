/*
 * probe-signal.cpp
 *
 * リニアチャープ（3 kHz → 7 kHz）信号の生成と正規化相互相関による検出。
 *
 * チャープ信号に Hann 窓を適用し、AAC / Opus エンコードの帯域内
 * （3–7 kHz）に収まるよう設計している。
 * 相関検出の処理利得は約 24 dB（256 sample）で、
 * 加算合成 0.1f の低振幅でも確実に検出できる。
 * ピーク位置は放物線補間によりサブサンプル精度で推定する。
 */

#include "audio/probe-signal.hpp"

#include <algorithm>
#include <cmath>

namespace ods::audio {

	namespace {

		constexpr double kPi = 3.14159265358979323846;

	} // namespace

	// ============================================================
	// ProbeSignal
	// ============================================================

	ProbeSignal::ProbeSignal() {
		const int    N          = kLength;
		const double sr         = static_cast<double>(kSampleRate);
		const double T          = static_cast<double>(N - 1) / sr;
		const double sweep_rate = (kFreqEnd - kFreqStart) / T;

		waveform_.resize(static_cast<size_t>(N));
		double energy = 0.0;

		for (int n = 0; n < N; ++n) {
			const double t    = static_cast<double>(n) / sr;
			const double hann = 0.5 * (1.0 - std::cos(2.0 * kPi * n / (N - 1)));
			// 瞬時周波数 f(t) = kFreqStart + sweep_rate * t
			// 瞬時位相 φ(t) = 2π ∫ f(τ)dτ = 2π (kFreqStart·t + ½·sweep_rate·t²)
			const double phase = 2.0 * kPi * (kFreqStart * t + 0.5 * sweep_rate * t * t);

			const double sample               = static_cast<double>(kAmplitude) * hann * std::sin(phase);
			waveform_[static_cast<size_t>(n)] = static_cast<float>(sample);
			energy += sample * sample;
		}

		// L2 正規化リファレンス（相関計算の分子で使用）
		reference_.resize(static_cast<size_t>(N));
		const float inv_norm = static_cast<float>(1.0 / std::sqrt(energy));
		for (int n = 0; n < N; ++n) {
			reference_[static_cast<size_t>(n)] = waveform_[static_cast<size_t>(n)] * inv_norm;
		}
	}

	const std::vector<float> &ProbeSignal::waveform() const {
		return waveform_;
	}
	const std::vector<float> &ProbeSignal::reference() const {
		return reference_;
	}
	int ProbeSignal::length() const {
		return kLength;
	}

	// ============================================================
	// ProbeDetector
	// ============================================================

	ProbeDetector::ProbeDetector(const ProbeSignal &signal)
		: ref_(signal.reference()), ref_len_(signal.length()) {}

	void ProbeDetector::reset() {
		tail_.clear();
		has_tail_ = false;
	}

	ProbeDetector::DetectResult ProbeDetector::feed(const int16_t *samples, size_t count) {
		DetectResult result;
		if (count == 0) return result;

		// int16 → float 変換
		std::vector<float> current(count);
		for (size_t i = 0; i < count; ++i)
			current[i] = static_cast<float>(samples[i]) / 32768.0f;

		// テールバッファ + 現在チャンクを結合した作業バッファ
		const int tail_len = has_tail_ ? static_cast<int>(tail_.size()) : 0;
		const int work_len = tail_len + static_cast<int>(count);

		// 相関にはリファレンス長以上の連続サンプルが必要
		if (work_len < ref_len_) {
			// 短すぎる場合はテールに追加して次回に持ち越し
			if (has_tail_) {
				tail_.insert(tail_.end(), current.begin(), current.end());
			} else {
				tail_     = current;
				has_tail_ = true;
			}
			return result;
		}

		// 作業バッファ構築（テール + 現在チャンク）
		std::vector<float> work;
		work.reserve(static_cast<size_t>(work_len));
		if (has_tail_) work.insert(work.end(), tail_.begin(), tail_.end());
		work.insert(work.end(), current.begin(), current.end());

		// 正規化相互相関をスライド計算
		float best_corr = 0.0f;
		int   best_pos  = -1;

		const int num_positions = work_len - ref_len_ + 1;

		// 相関値を保存（放物線補間用、ピーク周辺のみ使うが全体を保持）
		std::vector<float> corr_vals(static_cast<size_t>(num_positions), 0.0f);

		for (int k = 0; k < num_positions; ++k) {
			float dot          = 0.0f;
			float local_energy = 0.0f;
			for (int i = 0; i < ref_len_; ++i) {
				const float w = work[static_cast<size_t>(k + i)];
				dot += ref_[static_cast<size_t>(i)] * w;
				local_energy += w * w;
			}
			// 無音区間はスキップ（ゼロ除算回避）
			if (local_energy < 1e-6f) continue;

			const float corr                  = dot / std::sqrt(local_energy);
			corr_vals[static_cast<size_t>(k)] = corr;
			if (corr > best_corr) {
				best_corr = corr;
				best_pos  = k;
			}
		}

		if (best_corr >= kDetectThreshold && best_pos >= 0) {
			result.detected = true;

			// 放物線ピーク補間でサブサンプル精度のピーク位置を算出
			double refined_pos = static_cast<double>(best_pos);
			if (best_pos > 0 && best_pos < num_positions - 1) {
				const float ym1   = corr_vals[static_cast<size_t>(best_pos - 1)];
				const float y0    = corr_vals[static_cast<size_t>(best_pos)];
				const float yp1   = corr_vals[static_cast<size_t>(best_pos + 1)];
				const float denom = ym1 - 2.0f * y0 + yp1;
				if (std::abs(denom) > 1e-9f) {
					const float delta = 0.5f * (ym1 - yp1) / denom;
					refined_pos += static_cast<double>(delta);
				}
			}

			// 作業バッファ上の位置を現在チャンク先頭基準に変換
			result.peak_offset = refined_pos - static_cast<double>(tail_len);
		}

		// 次回用テールバッファ: 現在チャンクの末尾 ref_len_-1 サンプルを保存
		const int needed = ref_len_ - 1;
		if (static_cast<int>(current.size()) >= needed) {
			tail_.assign(current.end() - needed, current.end());
		} else {
			// 現在チャンクが短い場合: テール末尾 + 現在チャンクから needed サンプルを確保
			std::vector<float> new_tail;
			new_tail.reserve(static_cast<size_t>(needed));
			if (has_tail_ && tail_len + static_cast<int>(current.size()) >= needed) {
				const int from_tail = needed - static_cast<int>(current.size());
				new_tail.insert(new_tail.end(),
								tail_.end() - from_tail,
								tail_.end());
			}
			new_tail.insert(new_tail.end(), current.begin(), current.end());
			tail_ = std::move(new_tail);
		}
		has_tail_ = true;

		return result;
	}

} // namespace ods::audio
