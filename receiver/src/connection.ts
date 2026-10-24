import { state } from './state';
import {
  MAGIC_AUDI,
  MAGIC_OPUS,
  PLAYBACK_BUFFER_DEFAULT,
  PLAYBACK_BUFFER_MIN_MS,
  PLAYBACK_BUFFER_MAX_MS,
  CONNECT_TIMEOUT_MS,
  DEFAULT_PING_COUNT,
} from './constants';
import {
  isRecord,
  isLatencyResultMessage,
  isTimingDiagramMessage,
  safeParseJson,
} from './types';
import { buildUrl, clearConnectTimer, resync } from './ui';
import {
  ensureAudioContext,
  handlePcm16,
  onPlaybackBufferChanged,
  ringActive,
  resetPlayback,
} from './audio';
import { handleOpus, sendPcmFallbackIfPossible } from './opus';
import { bus } from './bus';

// ============================================================
// 音声受信ディスパッチ
// ============================================================

function handleAudio(buf: ArrayBuffer): void {
  if (buf.byteLength < 16) return;
  const u32 = new Uint32Array(buf, 0, 4);
  const magic = u32[0];
  const sampleRate = u32[1],
    channels = u32[2],
    frameCount = u32[3];

  if (magic === MAGIC_AUDI) {
    handlePcm16(buf, sampleRate, channels, frameCount);
  } else if (magic === MAGIC_OPUS) {
    handleOpus(buf, sampleRate, channels, frameCount);
  }
}

// ============================================================
// 制御メッセージ
// ============================================================

function handleControl(text: string): void {
  const msg = safeParseJson(text);
  if (!isRecord(msg) || typeof msg.type !== 'string') return;

  switch (msg.type) {
    case 'session_info':
      bus.emit('ctrl:session', {
        streamId: typeof msg.stream_id === 'string' ? msg.stream_id : undefined,
        code: typeof msg.code === 'string' ? msg.code : undefined,
        memo: msg.memo,
      });
      break;

    case 'memo':
      bus.emit('ctrl:memo', { memo: msg.memo });
      break;

    case 'ping':
      if (state.ws && state.ws.readyState === WebSocket.OPEN) {
        const payload: { type: string; seq?: number } = { type: 'pong' };
        if (typeof msg.seq === 'number') payload.seq = msg.seq;
        state.ws.send(JSON.stringify(payload));
      }
      if (typeof msg.total === 'number' && msg.total > 0) state.pingTotal = msg.total;
      bus.emit('ctrl:ping', { count: ++state.pingCount });
      break;

    case 'latency_result':
      if (isLatencyResultMessage(msg)) {
        bus.emit('ctrl:latency', msg);
      }
      state.pingCount = 0;
      state.pingTotal = DEFAULT_PING_COUNT;
      break;

    case 'timing_diagram':
      if (isTimingDiagramMessage(msg)) {
        bus.emit('ctrl:timing_diagram', msg);
      }
      break;

    case 'playback_buffer':
      {
        const raw = typeof msg.ms === 'number' ? msg.ms : Number(msg.ms);
        if (!Number.isFinite(raw)) break;
        const clamped = Math.min(PLAYBACK_BUFFER_MAX_MS, Math.max(PLAYBACK_BUFFER_MIN_MS, raw));
        const prev = state.playbackBuffer;
        state.playbackBuffer = clamped / 1000;
        if (Math.abs(state.playbackBuffer - prev) > 0.001) {
          if (ringActive()) onPlaybackBufferChanged();
          else resync();
        }
      }
      break;
  }
}

// ============================================================
// WebSocket 接続
// ============================================================

const hostDomain = location.hostname || 'localhost';

// 本文はサーバーの StreamRouter::on_http（/memo）の 404 応答と対応する。
async function diagnoseRejection(sid: string, code: string): Promise<string | null> {
  try {
    const r = await fetch(
      `/memo?sid=${encodeURIComponent(sid)}&code=${encodeURIComponent(code)}`,
      { cache: 'no-store' },
    );
    if (r.status !== 404) return null;
    const body = await r.text();
    if (body === 'Code Not Found') return 'code_not_found';
    if (body === 'Not Found') return 'stream_id_mismatch';
  } catch {
    /* 判定できなければ通常の接続エラーとして扱う */
  }
  return null;
}

export function connect(): void {
  if (state.connecting) return;
  if (state.ws) state.ws.close();

  if (!ensureAudioContext()) {
    bus.emit('connect:rejected', { reason: 'no-audio' });
    return;
  }
  state.playbackBuffer = PLAYBACK_BUFFER_DEFAULT;
  resetPlayback();

  const sid = state.streamId;
  const code = state.channelCode;

  if (!sid) {
    bus.emit('connect:rejected', { reason: 'no-sid' });
    return;
  }
  if (!/^[a-z0-9]+$/i.test(sid)) {
    bus.emit('connect:rejected', { reason: 'invalid-sid' });
    return;
  }
  if (!code) {
    bus.emit('connect:rejected', { reason: 'no-code' });
    return;
  }

  const url = buildUrl(hostDomain, sid, code);
  state.connecting = true;
  state.closeReason = null;

  try {
    state.ws = new WebSocket(url);
  } catch {
    state.connecting = false;
    bus.emit('connect:rejected', { reason: 'ws-failed' });
    return;
  }

  bus.emit('ws:connecting', { url });
  state.ws.binaryType = 'arraybuffer';
  const wsLocal = state.ws;

  clearConnectTimer();
  state.connectTimer = setTimeout(() => {
    if (state.ws !== wsLocal) return;
    if (wsLocal.readyState !== WebSocket.OPEN) {
      state.closeReason = 'timeout';
      try {
        wsLocal.close();
      } catch {
        /* ignore */
      }
    }
  }, CONNECT_TIMEOUT_MS);

  let opened = false;
  wsLocal.onopen = () => {
    if (state.ws !== wsLocal) return;
    opened = true;
    clearConnectTimer();
    state.connecting = false;
    bus.emit('ws:open', { url });
    sendPcmFallbackIfPossible();
  };
  wsLocal.onerror = () => {
    if (state.ws !== wsLocal) return;
    if (!state.closeReason) state.closeReason = 'error';
  };
  wsLocal.onclose = (ev) => {
    if (state.ws !== wsLocal) return;
    clearConnectTimer();
    state.connecting = false;
    const cause = state.closeReason || 'server';
    state.closeReason = null;
    state.ws = null;
    if (!opened && cause === 'error') {
      // サーバーは不正な接続を Upgrade 前に HTTP 応答で拒否するため、ブラウザーには理由が
      // 届かない。/memo で理由を判定し、従来の close reason と同じ形で通知する。
      void diagnoseRejection(sid, code).then((reason) => {
        if (state.ws || state.connecting) return; // 判定中に再接続した
        bus.emit(
          'ws:close',
          reason ? { code: 1008, reason, cause: 'server' } : { code: ev?.code, reason: ev?.reason, cause },
        );
      });
      return;
    }
    bus.emit('ws:close', { code: ev?.code, reason: ev?.reason, cause });
  };
  wsLocal.onmessage = (ev: MessageEvent) => {
    if (state.ws !== wsLocal) return;
    if (ev.data instanceof ArrayBuffer) handleAudio(ev.data);
    else if (typeof ev.data === 'string') handleControl(ev.data);
  };
}

export function disconnect(): void {
  clearConnectTimer();
  state.connecting = false;
  state.closeReason = null;
  const wsLocal = state.ws;
  state.ws = null;
  bus.emit('ws:close', { cause: 'user' });
  if (wsLocal) {
    try {
      wsLocal.close();
    } catch {
      /* ignore */
    }
  }
}
