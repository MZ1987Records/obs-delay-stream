# src の時間関連主要データ構造（UML）

`src` 配下で「時間（delay / latency / RTT / playback buffer）」を扱う主要なデータ構造を、
mermaid の `classDiagram` で整理したものです。

```mermaid
classDiagram
    direction LR

    class DelayBuffer {
        +sample_rate_ : uint32_t
        +channels_ : uint32_t
        +delay_ms_ : uint32_t
        +delay_samples_ : size_t (atomic)
        +write_pos_ : size_t
        +buf_ : vector~float~
        +init(sample_rate, channels, max_delay_ms)
        +set_delay_ms(ms)
        +get_delay_ms() uint32_t
        +process(input, output, frames)
    }

    class LatencyResult {
        +valid : bool
        +avg_rtt_ms : double
        +avg_latency_ms : double
        +min_rtt_ms : double
        +max_rtt_ms : double
        +samples : int
        +used_samples : int
        +method : const char*
    }

    class RtmpProbeResult {
        +valid : bool
        +avg_rtt_ms : double
        +avg_latency_ms : double
        +min_rtt_ms : double
        +max_rtt_ms : double
        +jitter_ms : double
        +samples : int
        +failed : int
        +error_msg : string
    }

    class ChannelState {
        +ping_times : map~int, Clock::time_point~
        +rtt_samples : vector~double~
        +measuring : bool (atomic)
        +last_applied_delay : double
        +last_applied_reason : string
    }

    class ChSummary {
        +ch : int
        +connected : bool
        +measured : bool
        +one_way_latency_ms : double
    }

    class FlowResult {
        +completed_count : int
        +ping_sent_count : int
        +ping_total_count : int
        +rtsp_e2e_latency_ms : double
        +rtsp_e2e_valid : bool
        +rtsp_e2e_error : string
        +max_latency_ms() double
        +ch_measured_ms(ch_index) int
        +rtsp_e2e_ms() int
    }

    class MeasureState {
        +measuring_ : bool
        +applied_ : bool
        +last_error_ : string
    }

    class RtmpMeasureState {
        +prober : RtmpProber
        +applied_ : bool
        +cached_url_ : string
    }

    class SubChannel {
        +measured_ms : int
        +offset_ms : int
    }

    class DelayStreamData {
        +playback_buffer_ms : int
        +avatar_latency_ms : int
        +measured_rtsp_e2e_ms : int
    }

    class AudioConfig {
        +playback_buffer_ms : int
    }

    class SyncFlow {
        +phase_ : FlowPhase
        +ping_count_ : int
        +prober_ : RtmpProber
    }

    class StreamRouter {
        +playback_buffer_ms_ : int (atomic)
        +set_audio_config(cfg)
        +start_measurement(ch, num_pings, interval_ms, start_delay_ms)
    }

    DelayStreamData *-- DelayBuffer : master_buf
    DelayStreamData *-- "MAX_SUB_CH" SubChannel : sub_channels
    DelayStreamData *-- RtmpMeasureState : rtmp_measure
    DelayStreamData *-- SyncFlow : flow
    DelayStreamData *-- StreamRouter : router

    SubChannel *-- DelayBuffer : buf
    SubChannel *-- MeasureState : measure
    MeasureState --> LatencyResult : result_

    RtmpMeasureState --> RtmpProbeResult : result_

    FlowResult *-- "MAX_SUB_CH" ChSummary : channels
    SyncFlow --> FlowResult : result_

    StreamRouter o-- "sid/chごと" ChannelState : ch_map_
    ChannelState --> LatencyResult : last_result
    StreamRouter ..> AudioConfig
```

## 補足

- `DelayStreamData` がランタイム中の設定値（`avatar_latency_ms`, `measured_rtsp_e2e_ms`, `playback_buffer_ms`）と、各機能（`SyncFlow`, `StreamRouter`, `RtmpMeasureState`）を集約します。
- 実際の音声遅延適用は `DelayBuffer`（`master_buf` と各 `SubChannel::buf`）で行われます。
- 計測値は WebSocket 側が `LatencyResult`（チャンネル別）、RTSP E2E 側が `RtspE2eResult`、全体集約が `FlowResult` です。

## 遅延計算式

`recalc_all_delays()` で全チャンネル一括実行します。

```
R = measured_rtsp_e2e_ms      (RTSP E2E 計測結果)
A = avatar_latency_ms         (アバターレイテンシ)
B = playback_buffer_ms        (再生バッファ量)
C[i] = sub_channels[i].measured_ms  (チャンネル i の WS 計測結果)
offset[i] = sub_channels[i].offset_ms  (チャンネル i の手動補正)

raw[i]   = R - A - C[i] - B + offset[i]
neg_max  = max(0, max(-raw[i] for all measured i where raw[i] < 0))

ch_delay[i] = raw[i] + neg_max   (各チャンネルの DelayBuffer 適用値)
obs_delay   = neg_max             (OBS オーディオ出力の DelayBuffer 適用値)
```
