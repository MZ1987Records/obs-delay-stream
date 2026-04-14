export type JsonRecord = Record<string, unknown>;

export type AudioDecoderConfigLike = {
  codec: string;
  sampleRate: number;
  numberOfChannels: number;
};

export type EncodedAudioChunkInitLike = {
  type: 'key' | 'delta';
  timestamp: number;
  duration?: number;
  data: BufferSource;
};

// WebCodecs EncodedAudioChunk のオペーク型
// eslint-disable-next-line @typescript-eslint/no-empty-object-type
export type EncodedAudioChunkLike = {};

export type EncodedAudioChunkConstructorLike = new (
  init: EncodedAudioChunkInitLike,
) => EncodedAudioChunkLike;

export type AudioDataLike = {
  numberOfFrames: number;
  numberOfChannels: number;
  sampleRate: number;
  format?: string;
  copyTo: (destination: BufferSource, options: { planeIndex: number }) => void;
  close: () => void;
};

export type AudioDecoderLike = {
  configure: (config: AudioDecoderConfigLike) => void;
  decode: (chunk: EncodedAudioChunkLike) => void;
  close: () => void;
};

export type AudioDecoderConstructorLike = {
  new (init: {
    output: (audioData: AudioDataLike) => void;
    error: (err: unknown) => void;
  }): AudioDecoderLike;
  isConfigSupported: (
    config: AudioDecoderConfigLike,
  ) => Promise<{ supported: boolean }>;
};

export type OpusDecodedFrame = {
  channelData: Float32Array[];
  samplesDecoded: number;
  sampleRate?: number;
  errors?: Array<{ code?: number; message?: string }>;
};

export type OpusDecoderInstance = {
  decodeFrame: (data: Uint8Array) => OpusDecodedFrame | null;
  reset?: () => Promise<void>;
  free: () => void;
  ready: Promise<void>;
};

export type OpusDecoderModule = {
  OpusDecoder: new (opts: {
    sampleRate: number;
    channels: number;
  }) => OpusDecoderInstance;
};

export type WindowWithWebkitAudioContext = Window & {
  webkitAudioContext?: typeof AudioContext;
};

export type WindowWithOpusDecoder = Window & {
  'opus-decoder'?: OpusDecoderModule;
};

export type OpusPacket = {
  buf: ArrayBuffer;
  sampleRate: number;
  channels: number;
  frameCount: number;
};

export type StatusClass = '' | 'ok' | 'err' | 'mea';

export type LatencyResultMessage = {
  one_way: number;
  avg_rtt: number;
  min: number;
  max: number;
};

export type TimingDiagramMessage = {
  R: number;
  A: number;
  buf: number;
  master_delay: number;
  ch_measured_ms: number;
  ch_total_ms: number;
  ch_offset_ms: number;
  ch_provisional: boolean;
};

export type ShebangParams = { sid: string | null; code: string | null };
export type ConfigResponse = { active_ch?: number };
export type MemoResponse = { memo?: string };

export function isRecord(value: unknown): value is JsonRecord {
  return typeof value === 'object' && value !== null;
}

export function safeParseJson(text: string): unknown {
  try {
    return JSON.parse(text);
  } catch {
    return null;
  }
}

export function isLatencyResultMessage(
  value: JsonRecord,
): value is LatencyResultMessage {
  return (
    typeof value.one_way === 'number' &&
    typeof value.avg_rtt === 'number' &&
    typeof value.min === 'number' &&
    typeof value.max === 'number'
  );
}

export function isTimingDiagramMessage(
  value: JsonRecord,
): value is TimingDiagramMessage {
  return (
    typeof value.R === 'number' &&
    typeof value.A === 'number' &&
    typeof value.buf === 'number' &&
    typeof value.master_delay === 'number' &&
    typeof value.ch_measured_ms === 'number' &&
    typeof value.ch_total_ms === 'number' &&
    typeof value.ch_offset_ms === 'number' &&
    typeof value.ch_provisional === 'boolean'
  );
}

export function isConfigResponse(value: unknown): value is ConfigResponse {
  return (
    isRecord(value) &&
    (value.active_ch === undefined || Number.isInteger(value.active_ch))
  );
}

export function isMemoResponse(value: unknown): value is MemoResponse {
  return (
    isRecord(value) &&
    (value.memo === undefined || typeof value.memo === 'string')
  );
}
