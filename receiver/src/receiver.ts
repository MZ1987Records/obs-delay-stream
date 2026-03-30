import '@fortawesome/fontawesome-free/css/all.min.css';
import 'bulma/css/bulma.min.css';
import '../css/ui.css';
import { applyStaticI18n, initI18n, renderLanguageSwitcher, t } from './i18n';
import { DEFAULT_MAX_CH, DEFAULT_VOLUME } from './constants';
import {
  sidInput,
  chInput,
  volSlider,
  syncIntervalSelect,
  connectBtn,
  stopBtn,
  syncBtn,
  latencyCard,
} from './elements';
import { getOptionalElement } from './dom';
import { onVolumeChange, toggleMute, initMeter } from './audio';
import {
  applyChRange,
  applyUrlParams,
  updateBrowserWarning,
  loadConfig,
  updateUrlPreview,
  updateShebang,
  onSyncIntervalChange,
  resync,
  setStatus,
  setInputsEnabled,
  setDisconnectedUi,
  enableSyncOptions,
  showMeasuring,
  showLatencyResult,
  showApplied,
  setMeterOffline,
  updateMemoDisplay,
  getChRangeText,
} from './ui';
import { connect, disconnect } from './connection';
import { bus } from './bus';
import { state } from './state';

// ============================================================
// 初期化
// ============================================================

initI18n();

try {
  const buildTimestamp = (
    document.documentElement.dataset.buildTimestamp || ''
  ).trim();
  if (buildTimestamp && !/^@.+@$/.test(buildTimestamp)) {
    console.info('[obs-delay-stream] receiver build:', buildTimestamp);
  }
} catch {
  /* ignore */
}

applyStaticI18n();
renderLanguageSwitcher();

// DOM初期設定
applyChRange(DEFAULT_MAX_CH);
applyUrlParams();
updateBrowserWarning();
loadConfig();
initMeter();

// 音量・同期の初期値をlocalStorageから復元
{
  const savedVol = localStorage.getItem('volume');
  const initVol = savedVol !== null ? Number(savedVol) : DEFAULT_VOLUME;
  volSlider.value = String(initVol);
  onVolumeChange(initVol);

  const savedSync = localStorage.getItem('syncInterval');
  if (savedSync !== null) {
    syncIntervalSelect.value = savedSync;
    onSyncIntervalChange(savedSync);
  }
}

// ============================================================
// イベントリスナー
// ============================================================

function onInputChange(): void {
  updateUrlPreview();
  const sid = sidInput.value.trim();
  const ch = chInput.value || '1';
  updateShebang(sid, ch);
}

sidInput.addEventListener('input', onInputChange);
chInput.addEventListener('input', onInputChange);
updateUrlPreview();

// ============================================================
// イベントバス購読
// ============================================================

bus.on('connect:rejected', ({ reason }) => {
  const keys: Record<string, string> = {
    'no-audio': 'status.connectFailed',
    'no-sid': 'status.enterStreamId',
    'invalid-sid': 'status.invalidStreamId',
    'ws-failed': 'status.connectFailed',
  };
  setStatus(t(keys[reason]), 'err');
});

bus.on('ws:connecting', () => {
  setStatus(t('status.connecting'), '');
  setInputsEnabled(false);
  connectBtn.disabled = true;
});

bus.on('ws:open', ({ url }) => {
  setStatus(t('status.receivingWithUrl', { url }), 'ok');
  connectBtn.disabled = true;
  stopBtn.disabled = false;
  syncBtn.disabled = false;
  enableSyncOptions(true);
  latencyCard.classList.add('has-background-info-light');
  setMeterOffline(false);
});

bus.on('ws:close', ({ code, reason, cause }) => {
  if (cause === 'user') {
    setStatus(t('status.disconnectedByUser'), '');
  } else if (cause === 'timeout') {
    setStatus(t('status.connectTimeout'), 'err');
  } else if (code === 1008 && reason === 'stream_id_mismatch') {
    setStatus(t('status.streamIdMismatch'), 'err');
  } else if (code === 1008 && reason === 'ch_out_of_range') {
    setStatus(t('status.chOutOfRange', { range: getChRangeText() }), 'err');
  } else if (cause === 'error') {
    setStatus(t('status.connectionError'), 'err');
  } else {
    setStatus(t('status.disconnected'), '');
  }
  setDisconnectedUi();
});

bus.on('ctrl:session', ({ streamId, ch, memo }) => {
  if (typeof streamId === 'string') sidInput.value = streamId;
  if (ch !== undefined) {
    const chNum = Number(ch);
    if (Number.isFinite(chNum)) chInput.value = String(chNum);
  }
  const memoEl = getOptionalElement<HTMLElement>('infoMemo');
  if (memoEl) updateMemoDisplay(memoEl, memo);
});

bus.on('ctrl:memo', ({ memo }) => {
  const memoEl = getOptionalElement<HTMLElement>('infoMemo');
  if (memoEl) updateMemoDisplay(memoEl, memo);
});

bus.on('ctrl:ping', ({ count }) => showMeasuring(count));

bus.on('ctrl:latency', (r) => showLatencyResult(r));

bus.on('ctrl:delay', ({ ms, reason }) => {
  if (reason === 'auto_measure' || getOptionalElement<HTMLElement>('waitingApply')) {
    showApplied(ms);
  }
});

// ============================================================
// デバッグ: バッファオフセット調整 (?debug=1 で有効)
// ============================================================

if (new URLSearchParams(location.search).has('debug')) {
  const target = document.getElementById('infoBuf')?.parentElement;
  if (target) {
    const wrap = document.createElement('div');
    wrap.className = 'column is-full has-text-centered';
    wrap.innerHTML = [
      '<span class="tag is-dark is-small">Debug Playhead Shift</span> ',
      '<button class="button is-small is-danger" id="dbgBack10">◀10</button> ',
      '<button class="button is-small is-danger is-outlined" id="dbgBack1">◀1</button> ',
      '<button class="button is-small is-success is-outlined" id="dbgFwd1">1▶</button> ',
      '<button class="button is-small is-success" id="dbgFwd10">10▶</button>',
    ].join('');
    target.parentElement?.appendChild(wrap);

    const shift = (ms: number) => { state.nextTime += ms / 1000; };
    document.getElementById('dbgBack10')!.onclick = () => shift(-10);
    document.getElementById('dbgBack1')!.onclick = () => shift(-1);
    document.getElementById('dbgFwd1')!.onclick = () => shift(1);
    document.getElementById('dbgFwd10')!.onclick = () => shift(10);
  }
}

// ============================================================
// グローバル公開 (HTML onclick 用)
// ============================================================

Object.assign(window, {
  connect,
  disconnect,
  resync,
  onSyncIntervalChange,
  toggleMute,
  onVolumeChange,
});
