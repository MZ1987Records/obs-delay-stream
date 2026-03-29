import { state } from './state';
import {
  MIN_CH,
  PING_SAMPLES,
  WS_PORT,
  RESYNC_DISPLAY_MS,
  RESYNC_RAMP_MAX_MS,
  RESYNC_RAMP_MIN_DRIFT_MS,
  RESYNC_RAMP_MS,
  AHEAD,
} from './constants';
import {
  statusBar,
  statusText,
  statusIcon,
  infoCodec,
  latencyCard,
  latencyContent,
  sidInput,
  chInput,
  chHint,
  urlPreview,
  browserWarningBlock,
  connectBtn,
  stopBtn,
  syncBtn,
  syncBtnLabel,
  syncIntervalSelect,
} from './elements';
import { t, tr } from './i18n';
import { getOptionalElement, h } from './dom';
import type {
  LatencyResultMessage,
  ShebangParams,
  StatusClass,
} from './types';
import { isRecord, isConfigResponse, isMemoResponse } from './types';

// ============================================================
// ステータスバー
// ============================================================

const STATUS_CLASS_MAP: Record<string, string> = {
  ok: 'is-success',
  err: 'is-danger',
  mea: 'is-warning',
};

const STATUS_ICON_MAP: Record<string, string> = {
  ok: 'fa-check-circle',
  err: 'fa-times-circle',
  mea: 'fa-exclamation-triangle',
};

export function setStatus(msg: string, cls: StatusClass = ''): void {
  if (statusText) statusText.textContent = msg;
  const bulmaClass = STATUS_CLASS_MAP[cls] || 'is-dark';
  statusBar.className = `notification ${bulmaClass}`;
  const icon = STATUS_ICON_MAP[cls] || 'fa-info-circle';
  if (statusIcon) statusIcon.className = `fas ${icon}`;
}

export function setCodecLabel(text?: string): void {
  infoCodec.textContent = text || '—';
}

// ============================================================
// URL プレビュー / ブラウザ判定
// ============================================================

const hostDomain = location.hostname || 'localhost';

export function buildUrl(
  ip: string,
  sid: string,
  ch: string | number,
): string {
  const hasScheme = /^(wss?|https?):\/\//.test(ip);
  const isTunnel =
    ip.includes('trycloudflare.com') ||
    hasScheme ||
    location.protocol === 'https:';
  const cleanIp = ip.replace(/^(wss?|https?):\/\//, '').replace(/\/.*$/, '');
  const sidPart = sid || t('url.streamIdMissing');
  if (isTunnel) {
    return `wss://${cleanIp}/${sidPart}/${ch}`;
  }
  return `ws://${cleanIp}:${WS_PORT}/${sidPart}/${ch}`;
}

export function updateUrlPreview(): void {
  const sid = sidInput.value.trim();
  const ch = chInput.value || '1';
  if (urlPreview) urlPreview.textContent = buildUrl(hostDomain, sid, ch);
}

export function updateShebang(sid: string, ch: string): void {
  if (sid) {
    const sidEnc = encodeURIComponent(sid);
    const chEnc = encodeURIComponent(ch || '1');
    const newHash = `#!/${sidEnc}/${chEnc}`;
    if (location.hash !== newHash) {
      history.replaceState(null, '', newHash);
    }
  } else if (location.hash.startsWith('#!')) {
    history.replaceState(null, '', location.pathname + location.search);
  }
}

function parseShebangParams(): ShebangParams | null {
  if (!location.hash.startsWith('#!')) return null;
  let raw = location.hash.slice(2);
  if (raw.startsWith('/')) raw = raw.slice(1);
  if (raw.startsWith('?')) raw = raw.slice(1);
  if (!raw) return null;

  const safeDecode = (val: string): string => {
    try {
      return decodeURIComponent(val);
    } catch {
      return val;
    }
  };

  const base = raw.split(/[?#]/)[0];
  const parts = base.split('/');
  return {
    sid: parts[0] ? safeDecode(parts[0]) : null,
    ch: parts.length >= 2 ? safeDecode(parts[1]) : null,
  };
}

function getInitialParams(): ShebangParams | null {
  const hashParams = parseShebangParams();
  if (hashParams && (hashParams.sid || hashParams.ch)) return hashParams;
  return null;
}

export function applyUrlParams(): void {
  const params = getInitialParams();
  if (!params) return;
  const { sid, ch } = params;
  if (sid) sidInput.value = sid;
  if (ch) {
    const chNum = parseInt(ch, 10);
    if (!Number.isNaN(chNum) && chNum >= MIN_CH && chNum <= state.maxCh) {
      chInput.value = String(chNum);
    }
  }
  if (sid && ch) fetchMemoPreview(sid, ch);
}

export function isChromeBrowser(): boolean {
  const uaData = (
    navigator as Navigator & {
      userAgentData?: { brands?: Array<{ brand: string }> };
    }
  ).userAgentData;
  if (uaData && Array.isArray(uaData.brands)) {
    return uaData.brands.some((brand) => brand.brand === 'Google Chrome');
  }
  const ua = navigator.userAgent || '';
  const hasChromeToken = /Chrome\/\d+/.test(ua) || /CriOS\/\d+/.test(ua);
  if (!hasChromeToken) return false;
  const nonChromeToken =
    /(Edg|OPR|Opera|SamsungBrowser|UCBrowser|YaBrowser|Vivaldi)\//.test(ua);
  return !nonChromeToken;
}

export function updateBrowserWarning(): void {
  if (!browserWarningBlock) return;
  browserWarningBlock.hidden = isChromeBrowser();
}

// ============================================================
// 設定取得 / メモ
// ============================================================

export function getChRangeText(): string {
  return t('format.chRange', { min: MIN_CH, max: state.maxCh });
}

export function applyChRange(max: number): void {
  state.maxCh = max;
  if (chHint) chHint.textContent = `(${getChRangeText()})`;
  chInput.min = String(MIN_CH);
  chInput.max = String(max);
  const v = parseInt(chInput.value, 10);
  if (Number.isInteger(v) && v > max) chInput.value = String(max);
  updateUrlPreview();
}

export function loadConfig(): void {
  fetch('/config', { cache: 'no-store' })
    .then((r) => (r.ok ? r.json() : null))
    .then((cfg) => {
      if (!cfg || !isConfigResponse(cfg)) return;
      const v = cfg.active_ch;
      if (typeof v === 'number' && Number.isInteger(v) && v >= MIN_CH)
        applyChRange(v);
    })
    .catch(() => {});
}

export function fetchMemoPreview(sid: string, ch: string): void {
  const chNum = parseInt(ch, 10);
  if (!sid || !Number.isInteger(chNum) || chNum < MIN_CH || chNum > state.maxCh)
    return;
  const memoEl = getOptionalElement<HTMLElement>('infoMemo');
  if (!memoEl) return;
  const url = `/memo?sid=${encodeURIComponent(sid)}&ch=${encodeURIComponent(String(chNum))}`;
  fetch(url, { cache: 'no-store' })
    .then((r) => (r.ok ? r.json() : null))
    .then((data) => {
      if (!data || !isMemoResponse(data) || typeof data.memo !== 'string')
        return;
      updateMemoDisplay(memoEl, data.memo);
    })
    .catch(() => {});
}

export function updateMemoDisplay(
  memoEl: HTMLElement | null,
  memoText: unknown,
): void {
  if (!memoEl) return;
  const text = typeof memoText === 'string' ? memoText.trim() : '';
  if (!text.length) {
    memoEl.textContent = '';
    memoEl.hidden = true;
    return;
  }
  memoEl.textContent = text;
  memoEl.hidden = false;
}

// ============================================================
// 接続UI状態管理
// ============================================================

export function clearConnectTimer(): void {
  if (state.connectTimer) {
    clearTimeout(state.connectTimer);
    state.connectTimer = null;
  }
}

export function setInputsEnabled(enabled: boolean): void {
  sidInput.disabled = !enabled;
  chInput.disabled = !enabled;
}

export function setDisconnectedUi(): void {
  connectBtn.disabled = false;
  stopBtn.disabled = true;
  syncBtn.disabled = true;
  enableSyncOptions(false);
  latencyCard.classList.remove('has-background-info-light');
  setInputsEnabled(true);
  setCodecLabel('—');
}

// ============================================================
// 遅延計測UI
// ============================================================

export function showMeasuring(n: number): void {
  const statusMsg = tr(
    'status.measuring',
    { current: n, total: PING_SAMPLES },
    '遅延計測中... ({{current}}/{{total}})',
    'Measuring latency... ({{current}}/{{total}})',
  );
  const measuringText = tr(
    'latency.measuring',
    { current: n, total: PING_SAMPLES },
    '計測中 ({{current}} / {{total}} ping)',
    'Measuring ({{current}} / {{total}} ping)',
  );
  setStatus(statusMsg, 'mea');
  latencyContent.textContent = '';
  latencyContent.appendChild(
    h('div', { class: 'measuring-box' },
      measuringText,
      h('span', { class: 'dots' },
        h('span', null, '.'), h('span', null, '.'), h('span', null, '.'),
      ),
      h('progress', {
        class: 'progress is-small is-warning',
        style: 'margin-top:6px',
        value: String(n),
        max: String(PING_SAMPLES),
      }, String(n)),
    ),
  );
}

export function showLatencyResult(r: LatencyResultMessage): void {
  setStatus(t('status.receiving'), 'ok');
  const colCls =
    r.avg_rtt < 50
      ? 'has-text-success'
      : r.avg_rtt < 150
        ? 'has-text-warning'
        : 'has-text-danger';
  const lbl =
    r.avg_rtt < 50
      ? tr('quality.good', {}, '良好', 'Good')
      : r.avg_rtt < 150
        ? tr('quality.normal', {}, '普通', 'Normal')
        : tr('quality.high', {}, '高遅延', 'High latency');
  const estimatedOneWay = tr(
    'latency.estimatedOneWay',
    {},
    '推定片道遅延 (RTT÷2)',
    'Estimated one-way latency (RTT/2)',
  );
  const avgRoundTrip = tr(
    'latency.averageRoundTrip',
    { label: lbl },
    '平均往復遅延 ({{label}})',
    'Average round-trip latency ({{label}})',
  );
  const minRtt = tr('latency.minRtt', {}, '最小RTT', 'Min RTT');
  const maxRtt = tr('latency.maxRtt', {}, '最大RTT', 'Max RTT');
  const waitingApply = tr(
    'latency.waitingApply',
    {},
    'OBSが遅延設定を反映するまでお待ちください。',
    'Please wait while OBS applies delay settings.',
  );
  const metricCell = (
    val: string, unit: string, label: string, valAttrs: Record<string, string>,
  ): HTMLElement =>
    h('div', { class: 'metric has-text-centered' },
      h('span', valAttrs, val),
      h('span', { class: 'unit has-text-grey' }, unit),
      h('div', { class: 'lbl has-text-grey' }, label),
    );

  const grid = h('div', { class: 'latency-grid' },
    metricCell(r.one_way.toFixed(1), 'ms', estimatedOneWay, { class: `val ${colCls}` }),
    metricCell(r.avg_rtt.toFixed(1), 'ms RTT', avgRoundTrip, { class: `val ${colCls}` }),
    metricCell(r.min.toFixed(1), 'ms', minRtt, { class: 'val', style: 'color:#888' }),
    metricCell(r.max.toFixed(1), 'ms', maxRtt, { class: 'val', style: 'color:#888' }),
  );

  latencyContent.textContent = '';
  latencyContent.append(
    grid,
    h('div', { class: 'notification is-info py-3 px-4 mt-3', id: 'waitingApply' },
      h('i', { class: 'fas fa-info-circle mr-1' }),
      waitingApply,
    ),
  );
  latencyCard.classList.add('has-background-info-light');
}

export function showApplied(ms: number | string): void {
  document.getElementById('waitingApply')?.remove();
  document.getElementById('appliedDelayNote')?.remove();
  const msText = `${Number.parseFloat(String(ms)).toFixed(1)} ms`;
  const prefix = tr('latency.appliedPrefix', {}, 'OBSがチャンネル遅延を ', 'OBS auto-set channel delay to ');
  const suffix = tr('latency.appliedSuffix', {}, ' に自動設定しました', '');
  latencyContent.appendChild(
    h('div', { class: 'notification is-success py-3 px-4 mt-3', id: 'appliedDelayNote' },
      h('i', { class: 'fas fa-check-circle mr-1' }),
      prefix,
      h('strong', null, msText),
      suffix,
    ),
  );
}

// ============================================================
// 再同期 / 自動同期
// ============================================================

export function resync(): void {
  if (state.currentSyncInterval > 0) {
    startAutoSync(state.currentSyncInterval);
  }

  if (!state.actx || !state.gainNode) return;
  const now = state.actx.currentTime;
  const bufferedSec = Math.max(0, state.nextTime - now);
  const driftSec = Math.abs(bufferedSec - AHEAD);
  const minDriftSec = RESYNC_RAMP_MIN_DRIFT_MS / 1000;
  if (driftSec < minDriftSec) {
    state.nextTime = now + AHEAD;
    setStatus(t('status.resynced'), 'ok');
    setTimeout(() => {
      if (state.ws && state.ws.readyState === WebSocket.OPEN)
        setStatus(t('status.receiving'), 'ok');
    }, RESYNC_DISPLAY_MS);
    return;
  }
  const rampOutSec = Math.min(bufferedSec, RESYNC_RAMP_MAX_MS / 1000);
  const rampInSec = RESYNC_RAMP_MS / 1000;
  const stopAt = now + rampOutSec;
  const gainParam = state.gainNode.gain;
  const currentGain = gainParam.value;

  gainParam.cancelScheduledValues(now);
  gainParam.setValueAtTime(currentGain, now);
  if (rampOutSec > 0) {
    gainParam.linearRampToValueAtTime(0, stopAt);
  } else {
    gainParam.setValueAtTime(0, now);
  }

  for (const src of state.activeSources) {
    try {
      src.stop(stopAt);
    } catch {
      /* already stopped */
    }
  }

  gainParam.setValueAtTime(0, stopAt);
  gainParam.linearRampToValueAtTime(currentGain, stopAt + rampInSec);
  state.nextTime = now + AHEAD;
  state.nextBufferRampIn = true;

  setStatus(t('status.resynced'), 'ok');
  setTimeout(() => {
    if (state.ws && state.ws.readyState === WebSocket.OPEN)
      setStatus(t('status.receiving'), 'ok');
  }, RESYNC_DISPLAY_MS);
}

function updateCountdown(): void {
  if (state.syncTickTimer && state.currentSyncInterval > 0) {
    syncBtnLabel.textContent = tr(
      'button.resyncCountdown',
      { sec: state.syncCountdown },
      '再同期（あと{{sec}}秒）',
      'Resync ({{sec}}s)',
    );
  } else {
    syncBtnLabel.textContent = tr('button.resync', {}, '再同期', 'Resync');
  }
}

export function startAutoSync(interval: number): void {
  stopAutoSync();
  if (interval <= 0) return;
  state.currentSyncInterval = interval;
  state.syncCountdown = interval;
  updateCountdown();
  state.syncTickTimer = setInterval(() => {
    state.syncCountdown--;
    updateCountdown();
    if (state.syncCountdown <= 0) {
      resync();
    }
  }, 1000);
}

export function stopAutoSync(): void {
  if (state.syncTickTimer) {
    clearInterval(state.syncTickTimer);
    state.syncTickTimer = null;
  }
  state.currentSyncInterval = 0;
  updateCountdown();
}

export function onSyncIntervalChange(interval: number | string): void {
  const value = Number(interval);
  localStorage.setItem('syncInterval', String(value));
  state.pendingSyncInterval = Number.isFinite(value) ? value : 0;
  if (state.pendingSyncInterval === 0) {
    stopAutoSync();
    return;
  }
  if (state.syncAvailable) {
    startAutoSync(state.pendingSyncInterval);
  }
}

export function enableSyncOptions(enabled: boolean): void {
  state.syncAvailable = enabled;
  if (!enabled) {
    stopAutoSync();
    return;
  }
  onSyncIntervalChange(syncIntervalSelect.value);
}
