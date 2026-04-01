import { state } from './state';
import {
  MIN_CH,
  WS_PORT,
  RESYNC_DISPLAY_MS,
  RESYNC_XFADE_MIN_SEC,
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
  urlPreview,
  browserWarningBlock,
  connectBtn,
  stopBtn,
  syncBtn,
  syncBtnLabel,
  syncIntervalSelect,
  meterEl,
} from './elements';
import { t, tr } from './i18n';
import { getOptionalElement, h } from './dom';
import type {
  LatencyResultMessage,
  ShebangParams,
  StatusClass,
} from './types';
import { isRecord, isConfigResponse, isMemoResponse } from './types';
import { scheduleSustain } from './sustain';

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
  code: string,
): string {
  const hasScheme = /^(wss?|https?):\/\//.test(ip);
  const isTunnel =
    ip.includes('trycloudflare.com') ||
    hasScheme ||
    location.protocol === 'https:';
  const cleanIp = ip.replace(/^(wss?|https?):\/\//, '').replace(/\/.*$/, '');
  const sidPart = sid || t('url.streamIdMissing');
  if (isTunnel) {
    return `wss://${cleanIp}/${sidPart}/${code}`;
  }
  return `ws://${cleanIp}:${WS_PORT}/${sidPart}/${code}`;
}

export function updateUrlPreview(): void {
  const sid = state.streamId;
  const code = state.channelCode;
  if (urlPreview) {
    urlPreview.textContent = (sid && code)
      ? buildUrl(hostDomain, sid, code)
      : '';
  }
}

export function updateShebang(sid: string, code: string): void {
  if (sid && code) {
    const sidEnc = encodeURIComponent(sid);
    const codeEnc = encodeURIComponent(code);
    const newHash = `#!/${sidEnc}/${codeEnc}`;
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
    code: parts.length >= 2 ? safeDecode(parts[1]) : null,
  };
}

function getInitialParams(): ShebangParams | null {
  const hashParams = parseShebangParams();
  if (hashParams && (hashParams.sid || hashParams.code)) return hashParams;
  return null;
}

export function applyUrlParams(): void {
  const params = getInitialParams();
  if (!params) return;
  const { sid, code } = params;
  if (sid) {
    state.streamId = sid;
    sidInput.value = sid;
  }
  if (code) {
    state.channelCode = code;
  }
  if (sid && code) fetchMemoPreview(sid, code);
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

export function fetchMemoPreview(sid: string, code: string): void {
  if (!sid || !code) return;
  const memoEl = getOptionalElement<HTMLElement>('infoMemo');
  const url = `/memo?sid=${encodeURIComponent(sid)}&code=${encodeURIComponent(code)}`;
  fetch(url, { cache: 'no-store' })
    .then((r) => (r.ok ? r.json() : null))
    .then((data) => {
      if (!data || !isMemoResponse(data)) return;
      if (typeof data.ch === 'number' && Number.isFinite(data.ch)) {
        chInput.value = String(data.ch);
      }
      if (memoEl && typeof data.memo === 'string') {
        updateMemoDisplay(memoEl, data.memo);
      }
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

export function setInputsEnabled(_enabled: boolean): void {
  // sidInput/chInput は常に読み取り専用（URLから取得・サーバから受信）
}

export function setMeterOffline(offline: boolean): void {
  meterEl.classList.toggle('is-offline', offline);
}

export function setDisconnectedUi(): void {
  connectBtn.disabled = false;
  stopBtn.disabled = true;
  syncBtn.disabled = true;
  enableSyncOptions(false);
  latencyCard.classList.remove('has-background-info-light');
  setInputsEnabled(true);
  setCodecLabel('—');
  setMeterOffline(true);
}

// ============================================================
// 遅延計測UI
// ============================================================

export function showMeasuring(n: number): void {
  const statusMsg = tr(
    'status.measuring',
    { current: n, total: state.pingTotal },
    '遅延計測中... ({{current}}/{{total}})',
    'Measuring latency... ({{current}}/{{total}})',
  );
  const measuringText = tr(
    'latency.measuring',
    { current: n, total: state.pingTotal },
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
        max: String(state.pingTotal),
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

  if (!state.actx) return;
  const now = state.actx.currentTime;
  const oldNextTime = state.nextTime;
  const newNextTime = now + state.playbackBuffer;
  state.nextTime = newNextTime;

  const overlapSec = oldNextTime - newNextTime;

  if (overlapSec > RESYNC_XFADE_MIN_SEC && state.xfade[0] && state.xfade[1]) {
    // 旧バッファと新バッファが重なる → クロスフェード
    // newNextTime〜oldNextTime の重複区間でフェードし、ギャップを作らない
    const oldIdx = state.xfadeIdx;
    const newIdx: 0 | 1 = oldIdx === 0 ? 1 : 0;
    state.xfadeIdx = newIdx;

    const fadeStart = newNextTime;
    const fadeEnd = oldNextTime;
    const oldGain = state.xfade[oldIdx]!.gain;
    const newGain = state.xfade[newIdx]!.gain;

    oldGain.cancelScheduledValues(now);
    oldGain.setValueAtTime(1, now);
    oldGain.linearRampToValueAtTime(1, fadeStart);
    oldGain.linearRampToValueAtTime(0, fadeEnd);

    newGain.cancelScheduledValues(now);
    newGain.setValueAtTime(0, now);
    newGain.linearRampToValueAtTime(0, fadeStart);
    newGain.linearRampToValueAtTime(1, fadeEnd);
  } else if (overlapSec < -RESYNC_XFADE_MIN_SEC && state.xfade[0] && state.xfade[1]) {
    // 旧バッファが先に途切れる → サステインで隙間を埋め、クロスフェードで新バッファへ遷移
    const oldIdx = state.xfadeIdx;
    const newIdx: 0 | 1 = oldIdx === 0 ? 1 : 0;
    state.xfadeIdx = newIdx;

    const gapStart = Math.max(oldNextTime, now);
    const gapDur = newNextTime - gapStart;
    const xfadeSec = Math.min(0.020, gapDur);

    // サステインを旧ノードにフルボリュームで接続、クロスフェード分だけ延長
    scheduleSustain(state.xfade[oldIdx]!, gapStart, gapDur + xfadeSec, false);

    // newNextTime でクロスフェード: サステイン(old)→0, 新バッファ(new)→1
    const oldGain = state.xfade[oldIdx]!.gain;
    const newGain = state.xfade[newIdx]!.gain;

    oldGain.cancelScheduledValues(now);
    oldGain.setValueAtTime(1, now);
    oldGain.linearRampToValueAtTime(1, newNextTime);
    oldGain.linearRampToValueAtTime(0, newNextTime + xfadeSec);

    newGain.cancelScheduledValues(now);
    newGain.setValueAtTime(0, now);
    newGain.linearRampToValueAtTime(0, newNextTime);
    newGain.linearRampToValueAtTime(1, newNextTime + xfadeSec);
  }

  if (state.pingCount === 0) {
    setStatus(t('status.resynced'), 'ok');
  }
  setTimeout(() => {
    if (state.ws && state.ws.readyState === WebSocket.OPEN && state.pingCount === 0)
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
