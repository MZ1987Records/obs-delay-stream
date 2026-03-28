import { state } from './state';
import type { OpusDecoderState } from './state';
import {
  WASM_ERROR_STREAK_WINDOW_MS,
  WASM_ERROR_STREAK_THRESHOLD,
} from './constants';
import type {
  AudioDataLike,
  AudioDecoderConfigLike,
  AudioDecoderConstructorLike,
  EncodedAudioChunkConstructorLike,
  OpusPacket,
  WindowWithOpusDecoder,
} from './types';
import { playBuffer, updateAudioInfo } from './audio';
import { setCodecLabel, setStatus } from './ui';
import { t } from './i18n';

// ============================================================
// エラー管理 / PCMフォールバック
// ============================================================

function reportOpusError(msg: string): void {
  if (state.opusErrorReported) return;
  state.opusErrorReported = true;
  setStatus(msg, 'err');
  console.warn('[Opus]', msg);
}

export function sendPcmFallbackIfPossible(): void {
  if (!state.pcmFallbackRequested) return;
  if (state.ws && state.ws.readyState === WebSocket.OPEN) {
    const payload: { type: string; mode: string; reason?: string } = {
      type: 'audio_codec',
      mode: 'pcm',
    };
    if (state.pcmFallbackReason) payload.reason = state.pcmFallbackReason;
    try {
      state.ws.send(JSON.stringify(payload));
    } catch {
      /* ws may be closing */
    }
  }
}

function requestPcmFallback(reason?: string): void {
  if (state.pcmFallbackRequested) return;
  state.pcmFallbackRequested = true;
  state.pcmFallbackReason = reason || '';
  sendPcmFallbackIfPossible();
}

function disableOpus(reason: string, msg?: string): void {
  if (state.opusErrored) return;
  state.opusErrored = true;
  requestPcmFallback(reason);
  reportOpusError(msg || t('codec.opusDecodeFailed'));
  state.opusQueue = [];
}

// ============================================================
// デコーダ状態管理
// ============================================================

function setWebCodecsState(
  newState: OpusDecoderState,
  reason?: string,
): void {
  state.opusWebCodecsState = newState;
  if (newState === 'failed' && reason) {
    console.warn('[Opus][WebCodecs]', reason);
  }
}

function setWasmState(newState: OpusDecoderState, reason?: string): void {
  state.opusWasmState = newState;
  if (newState === 'failed' && reason) {
    console.warn('[Opus][WASM]', reason);
  }
}

function noteWasmError(): number {
  const now = Date.now();
  if (now - state.opusWasmLastErrorTs > WASM_ERROR_STREAK_WINDOW_MS)
    state.opusWasmErrorStreak = 0;
  state.opusWasmLastErrorTs = now;
  state.opusWasmErrorStreak++;
  return state.opusWasmErrorStreak;
}

function clearWasmErrors(): void {
  state.opusWasmErrorStreak = 0;
  state.opusWasmLastErrorTs = 0;
}

// ============================================================
// WASM ライブラリ読み込み
// ============================================================

function loadOpusWasmLibrary(): Promise<unknown> {
  if (state.opusWasmLibPromise) return state.opusWasmLibPromise;
  state.opusWasmLibPromise = new Promise((resolve, reject) => {
    const s = document.createElement('script');
    s.async = true;
    s.src = 'opus-decoder/opus-decoder.min.js';
    s.onload = () => resolve(true);
    s.onerror = () => reject(new Error('opus-decoder load failed'));
    document.head.appendChild(s);
  });
  return state.opusWasmLibPromise;
}

// ============================================================
// WebCodecs デコーダ
// ============================================================

function ensureWebCodecsDecoder(
  sampleRate: number,
  channels: number,
): boolean {
  if (state.opusWebCodecsState === 'failed') return false;
  if (
    state.opusDecoder &&
    state.opusConfig &&
    state.opusConfig.sampleRate === sampleRate &&
    state.opusConfig.numberOfChannels === channels
  ) {
    state.opusWebCodecsState = 'ready';
    return true;
  }
  if (state.opusInitPromise) return false;
  if (state.opusDecoder) {
    try {
      state.opusDecoder.close();
    } catch {
      /* already closed */
    }
  }
  state.opusDecoder = null;
  state.opusConfig = null;
  state.opusTsUs = 0;

  if (!window.isSecureContext) {
    setWebCodecsState('failed', 'insecure_context');
    return false;
  }
  const AudioDecoderCtor = (
    window as unknown as { AudioDecoder?: AudioDecoderConstructorLike }
  ).AudioDecoder;
  if (!AudioDecoderCtor) {
    setWebCodecsState('failed', 'no_audio_decoder');
    return false;
  }

  const config: AudioDecoderConfigLike = {
    codec: 'opus',
    sampleRate,
    numberOfChannels: channels,
  };
  state.opusWebCodecsState = 'pending';
  state.opusInitPromise = AudioDecoderCtor.isConfigSupported(config)
    .then((s) => {
      if (!s.supported) {
        setWebCodecsState('failed', 'config_unsupported');
        return false;
      }
      state.opusDecoder = new AudioDecoderCtor({
        output: onOpusDecoded,
        error: () => {
          setWebCodecsState('failed', 'decode_error');
          state.opusMode = 'auto';
        },
      });
      state.opusDecoder.configure(config);
      state.opusConfig = config;
      state.opusTsUs = 0;
      state.opusWebCodecsState = 'ready';
      return true;
    })
    .then((ok) => {
      state.opusInitPromise = null;
      if (ok) {
        state.opusMode = 'webcodecs';
        flushOpusQueue();
        return true;
      }
      return false;
    })
    .catch(() => {
      state.opusInitPromise = null;
      setWebCodecsState('failed', 'init_failed');
      return false;
    });
  return false;
}

function decodeOpusPacketWebCodecs(
  buf: ArrayBuffer,
  sampleRate: number,
  channels: number,
  frameCount: number,
): void {
  if (!state.opusDecoder) return;
  updateAudioInfo(sampleRate, channels);

  const EncodedAudioChunkCtor = (
    window as unknown as { EncodedAudioChunk?: EncodedAudioChunkConstructorLike }
  ).EncodedAudioChunk;
  if (!EncodedAudioChunkCtor) {
    setWebCodecsState('failed', 'no_encoded_audio_chunk');
    return;
  }

  const payload = new Uint8Array(buf, 16);
  const durationUs =
    frameCount > 0 ? Math.round((frameCount * 1000000) / sampleRate) : 0;
  const chunk = new EncodedAudioChunkCtor({
    type: 'key',
    timestamp: state.opusTsUs,
    duration: durationUs,
    data: payload,
  });
  state.opusDecoder.decode(chunk);
  if (durationUs > 0) state.opusTsUs += durationUs;
}

function onOpusDecoded(ad: AudioDataLike): void {
  const abuf = audioDataToBuffer(ad);
  if (abuf) {
    setCodecLabel('Opus');
    playBuffer(abuf);
  }
  ad.close();
}

function audioDataToBuffer(ad: AudioDataLike): AudioBuffer | null {
  if (!state.actx) return null;
  const frames = ad.numberOfFrames;
  const channels = ad.numberOfChannels;
  const sampleRate = ad.sampleRate;
  const fmt = ad.format || '';
  const planar = fmt.endsWith('-planar');
  const f32 = fmt.startsWith('f32');
  const s16 = fmt.startsWith('s16');
  if (!f32 && !s16) {
    setWebCodecsState('failed', 'unsupported_audio_format');
    return null;
  }

  const abuf = state.actx.createBuffer(channels, frames, sampleRate);
  if (planar) {
    for (let c = 0; c < channels; c++) {
      if (f32) {
        const tmp = new Float32Array(frames);
        ad.copyTo(tmp, { planeIndex: c });
        abuf.getChannelData(c).set(tmp);
      } else {
        const tmp = new Int16Array(frames);
        ad.copyTo(tmp, { planeIndex: c });
        const out = abuf.getChannelData(c);
        for (let i = 0; i < frames; i++) out[i] = tmp[i] / 32768;
      }
    }
  } else {
    if (f32) {
      const tmp = new Float32Array(frames * channels);
      ad.copyTo(tmp, { planeIndex: 0 });
      for (let c = 0; c < channels; c++) {
        const out = abuf.getChannelData(c);
        for (let i = 0; i < frames; i++) out[i] = tmp[i * channels + c];
      }
    } else {
      const tmp = new Int16Array(frames * channels);
      ad.copyTo(tmp, { planeIndex: 0 });
      for (let c = 0; c < channels; c++) {
        const out = abuf.getChannelData(c);
        for (let i = 0; i < frames; i++)
          out[i] = tmp[i * channels + c] / 32768;
      }
    }
  }
  return abuf;
}

// ============================================================
// WASM デコーダ
// ============================================================

function ensureWasmDecoder(
  sampleRate: number,
  channels: number,
): boolean {
  if (state.opusWasmState === 'failed') return false;
  if (
    state.opusWasmDecoder &&
    state.opusWasmConfig &&
    state.opusWasmConfig.sampleRate === sampleRate &&
    state.opusWasmConfig.numberOfChannels === channels
  ) {
    state.opusWasmState = 'ready';
    return true;
  }
  if (state.opusWasmInitPromise) return false;
  if (state.opusWasmDecoder) {
    try {
      state.opusWasmDecoder.free();
    } catch {
      /* already freed */
    }
  }
  state.opusWasmDecoder = null;
  state.opusWasmConfig = null;

  state.opusWasmState = 'pending';
  state.opusWasmInitPromise = loadOpusWasmLibrary()
    .then(() => {
      const mod = (window as WindowWithOpusDecoder)['opus-decoder'];
      if (!mod || !mod.OpusDecoder)
        throw new Error('OpusDecoder missing');
      state.opusWasmDecoder = new mod.OpusDecoder({ sampleRate, channels });
      state.opusWasmConfig = { sampleRate, numberOfChannels: channels };
      return state.opusWasmDecoder.ready;
    })
    .then(() => {
      state.opusWasmInitPromise = null;
      state.opusWasmState = 'ready';
      state.opusMode = 'wasm';
      flushOpusQueue();
      return true;
    })
    .catch((e: unknown) => {
      state.opusWasmInitPromise = null;
      const msg =
        e && typeof e === 'object' && 'message' in e
          ? String((e as { message: unknown }).message)
          : 'init_failed';
      setWasmState('failed', msg);
      if (state.opusWebCodecsState === 'failed') {
        disableOpus('opus_unavailable', t('codec.opusUnavailable'));
      }
      return false;
    });
  return false;
}

function decodeOpusPacketWasm(
  buf: ArrayBuffer,
  sampleRate: number,
  channels: number,
  _frameCount: number,
): void {
  if (!state.opusWasmDecoder || !state.actx) return;
  updateAudioInfo(sampleRate, channels);

  const payload = new Uint8Array(buf, 16);
  let decoded;
  try {
    decoded = state.opusWasmDecoder.decodeFrame(payload);
  } catch {
    const streak = noteWasmError();
    console.warn('[Opus][WASM] decode_error');
    if (streak >= WASM_ERROR_STREAK_THRESHOLD) {
      setWasmState('failed', 'decode_error');
      if (state.opusWebCodecsState === 'failed') {
        disableOpus('opus_unavailable', t('codec.opusDecodeFailed'));
      } else {
        state.opusMode = 'auto';
      }
    } else if (
      !state.opusWasmInitPromise &&
      state.opusWasmDecoder &&
      typeof state.opusWasmDecoder.reset === 'function'
    ) {
      state.opusWasmState = 'pending';
      state.opusWasmInitPromise = state.opusWasmDecoder
        .reset()
        .then(() => {
          state.opusWasmInitPromise = null;
          state.opusWasmState = 'ready';
        })
        .catch(() => {
          state.opusWasmInitPromise = null;
          setWasmState('failed', 'reset_failed');
        });
    }
    return;
  }
  if (!decoded || !decoded.channelData || !decoded.samplesDecoded) return;
  if (decoded.errors && decoded.errors.length) {
    console.warn('[Opus][WASM] decode errors', decoded.errors[0]);
  }
  clearWasmErrors();

  const outRate = decoded.sampleRate || sampleRate;
  const outCh = decoded.channelData.length || channels;
  const abuf = state.actx.createBuffer(
    outCh,
    decoded.samplesDecoded,
    outRate,
  );
  for (let c = 0; c < outCh; c++) {
    const src = decoded.channelData[c];
    const out = abuf.getChannelData(c);
    if (src.length > decoded.samplesDecoded)
      out.set(src.subarray(0, decoded.samplesDecoded));
    else out.set(src);
  }
  setCodecLabel('Opus (WASM)');
  playBuffer(abuf);
}

// ============================================================
// パケットキュー / ルーティング
// ============================================================

function flushOpusQueue(): void {
  if (state.opusErrored) {
    state.opusQueue = [];
    return;
  }
  const q = state.opusQueue;
  state.opusQueue = [];
  for (const pkt of q) {
    if (state.opusMode === 'webcodecs') {
      decodeOpusPacketWebCodecs(
        pkt.buf,
        pkt.sampleRate,
        pkt.channels,
        pkt.frameCount,
      );
    } else if (state.opusMode === 'wasm') {
      decodeOpusPacketWasm(
        pkt.buf,
        pkt.sampleRate,
        pkt.channels,
        pkt.frameCount,
      );
    } else {
      handleOpus(pkt.buf, pkt.sampleRate, pkt.channels, pkt.frameCount);
    }
  }
}

function enqueue(pkt: OpusPacket): void {
  state.opusQueue.push(pkt);
}

export function handleOpus(
  buf: ArrayBuffer,
  sampleRate: number,
  channels: number,
  frameCount: number,
): void {
  if (state.opusErrored) return;
  const pkt: OpusPacket = { buf, sampleRate, channels, frameCount };

  // 明示的にWebCodecsモードの場合
  if (state.opusMode === 'webcodecs') {
    if (ensureWebCodecsDecoder(sampleRate, channels)) {
      decodeOpusPacketWebCodecs(buf, sampleRate, channels, frameCount);
      return;
    }
    if (state.opusWebCodecsState === 'failed') {
      state.opusMode = 'auto';
    } else {
      enqueue(pkt);
      return;
    }
  }

  // 明示的にWASMモードの場合
  if (state.opusMode === 'wasm') {
    if (ensureWasmDecoder(sampleRate, channels)) {
      decodeOpusPacketWasm(buf, sampleRate, channels, frameCount);
      return;
    }
    if (state.opusWasmState === 'failed') {
      state.opusMode = 'auto';
    } else {
      enqueue(pkt);
      return;
    }
  }

  // auto: WebCodecs優先、失敗時WASM
  if (ensureWebCodecsDecoder(sampleRate, channels)) {
    state.opusMode = 'webcodecs';
    decodeOpusPacketWebCodecs(buf, sampleRate, channels, frameCount);
    return;
  }
  if (state.opusWebCodecsState === 'pending') {
    enqueue(pkt);
    return;
  }
  if (ensureWasmDecoder(sampleRate, channels)) {
    state.opusMode = 'wasm';
    decodeOpusPacketWasm(buf, sampleRate, channels, frameCount);
    return;
  }
  if (state.opusWasmState === 'pending') {
    enqueue(pkt);
    return;
  }

  disableOpus('opus_unavailable', t('codec.opusUnavailable'));
}
