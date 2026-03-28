import { state } from './state';
import { MAGIC_AUDI, MAGIC_OPUS, AHEAD, CONNECT_TIMEOUT_MS } from './constants';
import { isRecord, isLatencyResultMessage, safeParseJson } from './types';
import type { JsonRecord } from './types';
import {
  sidInput,
  chInput,
  connectBtn,
  stopBtn,
  syncBtn,
  latencyCard,
} from './elements';
import { getOptionalElement } from './dom';
import { t } from './i18n';
import {
  setStatus,
  setCodecLabel,
  buildUrl,
  setInputsEnabled,
  setDisconnectedUi,
  clearConnectTimer,
  enableSyncOptions,
  stopAutoSync,
  showMeasuring,
  showLatencyResult,
  showApplied,
  updateMemoDisplay,
  getChRangeText,
} from './ui';
import { ensureAudioContext, handlePcm16 } from './audio';
import { handleOpus, sendPcmFallbackIfPossible } from './opus';

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
      if (typeof msg.stream_id === 'string') sidInput.value = msg.stream_id;
      if (msg.ch !== undefined && msg.ch !== null) {
        const chNum = Number(msg.ch);
        if (Number.isFinite(chNum)) chInput.value = String(chNum);
      }
      {
        const memoEl = getOptionalElement<HTMLElement>('infoMemo');
        if (memoEl) updateMemoDisplay(memoEl, msg.memo);
      }
      break;

    case 'memo':
      {
        const memoEl = getOptionalElement<HTMLElement>('infoMemo');
        if (memoEl) updateMemoDisplay(memoEl, msg.memo);
      }
      break;

    case 'ping':
      if (state.ws && state.ws.readyState === WebSocket.OPEN) {
        const payload: { type: string; seq?: number } = { type: 'pong' };
        if (typeof msg.seq === 'number') payload.seq = msg.seq;
        state.ws.send(JSON.stringify(payload));
      }
      showMeasuring(++state.pingCount);
      break;

    case 'latency_result':
      if (isLatencyResultMessage(msg as JsonRecord)) {
        showLatencyResult(
          msg as unknown as {
            one_way: number;
            avg_rtt: number;
            min: number;
            max: number;
          },
        );
      }
      state.pingCount = 0;
      break;

    case 'apply_delay':
      {
        const reason = typeof msg.reason === 'string' ? msg.reason : '';
        if (reason === 'manual_adjust') break;
        if (reason === 'auto_measure') {
          if (typeof msg.ms === 'number' || typeof msg.ms === 'string')
            showApplied(msg.ms as number | string);
          break;
        }
        // 旧サーバ互換
        if (getOptionalElement<HTMLElement>('waitingApply')) {
          if (typeof msg.ms === 'number' || typeof msg.ms === 'string')
            showApplied(msg.ms as number | string);
        }
      }
      break;
  }
}

// ============================================================
// WebSocket 接続
// ============================================================

const hostDomain = location.hostname || 'localhost';

export function connect(): void {
  if (state.connecting) return;
  if (state.ws) state.ws.close();

  if (!ensureAudioContext()) {
    setStatus(t('status.connectFailed'), 'err');
    return;
  }
  state.nextTime = state.actx!.currentTime + AHEAD;

  const sid = sidInput.value.trim();
  const ch = parseInt(chInput.value, 10);

  if (!sid) {
    setStatus(t('status.enterStreamId'), 'err');
    return;
  }
  if (!/^[a-z0-9]+$/i.test(sid)) {
    setStatus(t('status.invalidStreamId'), 'err');
    return;
  }

  const url = buildUrl(hostDomain, sid, ch);
  setStatus(t('status.connecting'), '');
  state.connecting = true;
  state.lastWsError = false;
  setInputsEnabled(false);
  connectBtn.disabled = true;
  try {
    state.ws = new WebSocket(url);
  } catch {
    state.connecting = false;
    setStatus(t('status.connectFailed'), 'err');
    connectBtn.disabled = false;
    setInputsEnabled(true);
    return;
  }
  state.ws.binaryType = 'arraybuffer';
  const wsLocal = state.ws;

  clearConnectTimer();
  state.connectTimer = setTimeout(() => {
    if (state.ws !== wsLocal) return;
    if (wsLocal.readyState !== WebSocket.OPEN) {
      state.lastWsError = true;
      setStatus(t('status.connectTimeout'), 'err');
      try {
        wsLocal.close();
      } catch {
        /* ignore */
      }
    }
  }, CONNECT_TIMEOUT_MS);

  wsLocal.onopen = () => {
    if (state.ws !== wsLocal) return;
    clearConnectTimer();
    state.connecting = false;
    setStatus(t('status.receivingWithUrl', { url }), 'ok');
    connectBtn.disabled = true;
    stopBtn.disabled = false;
    syncBtn.disabled = false;
    enableSyncOptions(true);
    latencyCard.classList.add('has-background-info-light');
    sendPcmFallbackIfPossible();
  };
  wsLocal.onerror = () => {
    if (state.ws !== wsLocal) return;
    state.lastWsError = true;
    setStatus(t('status.connectionError'), 'err');
  };
  wsLocal.onclose = (ev) => {
    if (state.ws !== wsLocal) return;
    clearConnectTimer();
    state.connecting = false;
    if (ev && ev.code === 1008 && ev.reason === 'stream_id_mismatch') {
      state.lastWsError = true;
      setStatus(t('status.streamIdMismatch'), 'err');
    } else if (ev && ev.code === 1008 && ev.reason === 'ch_out_of_range') {
      state.lastWsError = true;
      setStatus(t('status.chOutOfRange', { range: getChRangeText() }), 'err');
    } else if (!state.lastWsError) {
      setStatus(t('status.disconnected'), '');
    }
    state.lastWsError = false;
    setDisconnectedUi();
    state.ws = null;
  };
  wsLocal.onmessage = (ev: MessageEvent) => {
    if (state.ws !== wsLocal) return;
    if (ev.data instanceof ArrayBuffer) handleAudio(ev.data);
    else if (typeof ev.data === 'string') handleControl(ev.data);
  };
}

export function disconnect(): void {
  stopAutoSync();
  clearConnectTimer();
  state.connecting = false;
  syncBtn.disabled = true;
  enableSyncOptions(false);
  const wsLocal = state.ws;
  state.ws = null;
  setDisconnectedUi();
  setStatus(t('status.disconnectedByUser'), '');
  if (wsLocal) {
    try {
      wsLocal.close();
    } catch {
      /* ignore */
    }
  }
}
