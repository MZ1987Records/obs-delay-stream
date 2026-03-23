#pragma once
#include <vector>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <atomic>

// ============================================================
// DelayBuffer
//   サンプルレート・チャンネル数に対応したリングバッファ遅延器
//   遅延量はms単位でset_delay_ms()でいつでも変更可能
//
//   set_delay_ms() は任意のスレッドから呼び出し可能。
//   process() は音声スレッドから呼び出す。
//   delay_samples_ を atomic にすることで安全にクロススレッド変更可能。
// ============================================================
class DelayBuffer {
public:
    DelayBuffer() = default;

    // 初期化: サンプルレートとチャンネル数を設定
    // max_delay_ms: バッファ確保する最大遅延量(ms)
    // ※ 音声処理開始前、または音声スレッドから呼ぶこと
    void init(uint32_t sample_rate, uint32_t channels,
              uint32_t max_delay_ms = 10000)
    {
        sample_rate_ = sample_rate;
        channels_    = channels;
        size_t max_samples = (size_t)sample_rate * max_delay_ms / 1000 + 1;
        buf_.assign(max_samples * channels, 0.0f);
        write_pos_ = 0;
        set_delay_ms(0);
    }

    // 遅延量をms単位で設定（任意のスレッドから呼び出し可能）
    void set_delay_ms(uint32_t ms) {
        delay_ms_ = ms;
        size_t ds = (size_t)sample_rate_ * ms / 1000;
        // バッファサイズを超えないようにクランプ
        size_t max_samples = buf_.size() / std::max(channels_, 1u);
        if (ds > max_samples) ds = max_samples;
        delay_samples_.store(ds, std::memory_order_relaxed);
    }

    uint32_t get_delay_ms() const { return delay_ms_; }

    // frames フレーム分の音声を処理
    // input/output は interleaved float32, channels チャンネル
    // ※ 音声スレッドから呼ぶこと
    void process(const float* input, float* output, size_t frames) {
        if (buf_.empty() || channels_ == 0) {
            if (input != output)
                std::memcpy(output, input, frames * channels_ * sizeof(float));
            return;
        }
        size_t buf_frames = buf_.size() / channels_;
        // delay_samples_ のスナップショットをフレーム処理全体で使用
        size_t ds = delay_samples_.load(std::memory_order_relaxed);

        for (size_t i = 0; i < frames; ++i) {
            // 書き込み
            for (uint32_t c = 0; c < channels_; ++c)
                buf_[write_pos_ * channels_ + c] = input[i * channels_ + c];

            // 読み出し位置 = write_pos_ - delay_samples_
            size_t read_pos = (write_pos_ + buf_frames - ds) % buf_frames;
            for (uint32_t c = 0; c < channels_; ++c)
                output[i * channels_ + c] = buf_[read_pos * channels_ + c];

            write_pos_ = (write_pos_ + 1) % buf_frames;
        }
    }

private:
    uint32_t sample_rate_   = 48000;
    uint32_t channels_      = 2;
    uint32_t delay_ms_      = 0;
    std::atomic<size_t> delay_samples_{0};
    size_t   write_pos_     = 0;
    std::vector<float> buf_;
};
