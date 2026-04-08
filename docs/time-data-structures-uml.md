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
        +last_result : LatencyResult
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
        +channels : array~ChSummary, MAX_SUB_CH~
        +completed_count : int
        +ping_sent_count : int
        +ping_total_count : int
        +rtmp_latency_ms : double
        +rtmp_valid : bool
        +rtmp_error : string
        +max_latency_ms() double
        +proposed_sub_delay_ms(ch_index) double
        +proposed_master_delay_ms() double
    }

    class MeasureState {
        +result_ : LatencyResult
        +measuring_ : bool
        +applied_ : bool
        +last_error_ : string
    }

    class RtmpMeasureState {
        +prober : RtmpProber
        +result_ : RtmpProbeResult
        +applied_ : bool
        +cached_url_ : string
    }

    class SubChannel {
        +delay_ms : float
        +adjust_ms : float
        +buf : DelayBuffer
        +measure : MeasureState
    }

    class DelayStreamData {
        +playback_buffer_ms : int
        +master_base_delay_ms : float
        +master_offset_ms : float
        +master_buf : DelayBuffer
        +sub_channels : array~SubChannel, MAX_SUB_CH~
        +rtmp_measure : RtmpMeasureState
        +flow : SyncFlow
        +router : StreamRouter
    }

    class AudioConfig {
        +playback_buffer_ms : int
    }

    class SyncFlow {
        +phase_ : FlowPhase
        +ping_count_ : int
        +result_ : FlowResult
        +prober_ : RtmpProber
    }

    class StreamRouter {
        +playback_buffer_ms_ : int (atomic)
        +ch_map_ : map~string, ChannelState~
        +set_audio_config(cfg)
        +start_measurement(ch, num_pings, interval_ms, start_delay_ms)
    }

    DelayStreamData *-- DelayBuffer : master_buf
    DelayStreamData *-- "MAX_SUB_CH" SubChannel
    DelayStreamData *-- RtmpMeasureState
    DelayStreamData *-- SyncFlow
    DelayStreamData *-- StreamRouter

    SubChannel *-- DelayBuffer
    SubChannel *-- MeasureState
    MeasureState *-- LatencyResult

    RtmpMeasureState *-- RtmpProbeResult

    FlowResult *-- "MAX_SUB_CH" ChSummary
    SyncFlow *-- FlowResult

    StreamRouter *-- "sid/chごと" ChannelState
    ChannelState *-- LatencyResult
    StreamRouter ..> AudioConfig
```

## 補足

- `DelayStreamData` がランタイム中の時間系設定値（`master_base_delay_ms`, `master_offset_ms`, `playback_buffer_ms`）と、各機能（`SyncFlow`, `StreamRouter`, `RtmpMeasureState`）を集約します。
- 実際の音声遅延適用は `DelayBuffer`（`master_buf` と各 `SubChannel::buf`）で行われます。
- 計測値は WebSocket 側が `LatencyResult`、RTMP 側が `RtmpProbeResult`、最終的な提案値集約が `FlowResult` です。
