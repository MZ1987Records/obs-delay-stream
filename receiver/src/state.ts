import type {
  AudioDecoderConfigLike,
  AudioDecoderLike,
  OpusDecoderInstance,
  OpusPacket,
} from './types';
import { PLAYBACK_BUFFER_DEFAULT, DEFAULT_MAX_CH } from './constants';

export type OpusMode = 'auto' | 'webcodecs' | 'wasm';
export type OpusDecoderState = 'idle' | 'pending' | 'ready' | 'failed';

export const state = {
  // WebSocket
  ws: null as WebSocket | null,
  connecting: false,
  closeReason: null as null | 'timeout' | 'error',
  connectTimer: null as ReturnType<typeof setTimeout> | null,

  // AudioContext
  actx: null as AudioContext | null,
  gainNode: null as GainNode | null,
  xfade: [null, null] as [GainNode | null, GainNode | null],
  xfadeIdx: 0 as 0 | 1,
  lastBuffer: null as AudioBuffer | null,
  nextTime: 0,
  muted: false,
  playbackBuffer: PLAYBACK_BUFFER_DEFAULT,

  // CH範囲
  maxCh: DEFAULT_MAX_CH,

  // 同期
  syncAvailable: false,
  pingCount: 0,
  syncCountdown: 0,
  syncTickTimer: null as ReturnType<typeof setInterval> | null,
  currentSyncInterval: 0,
  pendingSyncInterval: 0,

  // Opus: WebCodecs
  opusDecoder: null as AudioDecoderLike | null,
  opusConfig: null as AudioDecoderConfigLike | null,
  opusInitPromise: null as Promise<boolean> | null,
  opusTsUs: 0,
  opusWebCodecsState: 'idle' as OpusDecoderState,

  // Opus: WASM
  opusWasmDecoder: null as OpusDecoderInstance | null,
  opusWasmConfig: null as { sampleRate: number; numberOfChannels: number } | null,
  opusWasmInitPromise: null as Promise<unknown> | null,
  opusWasmLibPromise: null as Promise<unknown> | null,
  opusWasmState: 'idle' as OpusDecoderState,
  opusWasmErrorStreak: 0,
  opusWasmLastErrorTs: 0,

  // Opus: 共通
  opusMode: 'auto' as OpusMode,
  opusQueue: [] as OpusPacket[],
  opusErrored: false,
  opusErrorReported: false,
  pcmFallbackRequested: false,
  pcmFallbackReason: '',
};
