#pragma once
#include <vector>
#include <cstring>
#include <cstdint>
#include <algorithm>

// ============================================================
// DelayBuffer
//   サンプルレート・チャンネル数に対応したリングバッファ遅延器
//   遅延量はms単位でset_delay_ms()でいつでも変更可能
// ============================================================
class DelayBuffer {
public:
    DelayBuffer() = default;

    // 初期化: サンプルレートとチャンネル数を設定
    // max_delay_ms: バッファ確保する最大遅延量(ms)
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

    // 遅延量をms単位で設定（スレッドセーフではないので音声スレッドから呼ぶこと）
    void set_delay_ms(uint32_t ms) {
        delay_ms_ = ms;
        delay_samples_ = (size_t)sample_rate_ * ms / 1000;
        // バッファサイズを超えないようにクランプ
        size_t max_samples = buf_.size() / std::max(channels_, 1u);
        if (delay_samples_ > max_samples) delay_samples_ = max_samples;
    }

    uint32_t get_delay_ms() const { return delay_ms_; }

    // frames フレーム分の音声を処理
    // input/output は interleaved float32, channels チャンネル
    void process(const float* input, float* output, size_t frames) {
        if (buf_.empty() || channels_ == 0) {
            if (input != output)
                std::memcpy(output, input, frames * channels_ * sizeof(float));
            return;
        }
        size_t buf_frames = buf_.size() / channels_;

        for (size_t i = 0; i < frames; ++i) {
            // 書き込み
            for (uint32_t c = 0; c < channels_; ++c)
                buf_[write_pos_ * channels_ + c] = input[i * channels_ + c];

            // 読み出し位置 = write_pos_ - delay_samples_
            size_t read_pos = (write_pos_ + buf_frames - delay_samples_) % buf_frames;
            for (uint32_t c = 0; c < channels_; ++c)
                output[i * channels_ + c] = buf_[read_pos * channels_ + c];

            write_pos_ = (write_pos_ + 1) % buf_frames;
        }
    }

private:
    uint32_t sample_rate_   = 48000;
    uint32_t channels_      = 2;
    uint32_t delay_ms_      = 0;
    size_t   delay_samples_ = 0;
    size_t   write_pos_     = 0;
    std::vector<float> buf_;
};
