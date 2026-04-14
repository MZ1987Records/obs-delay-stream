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
  codeInput,
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
  TimingDiagramMessage,
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
    codeInput.value = code;
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
  // sidInput/codeInput は常に読み取り専用（URLから取得・サーバから受信）
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
// レイテンシ計測UI
// ============================================================

type DiagramSegment = {
  ms: number;
  color: string;
};

type DiagramLane = {
  label: string;
  segments: DiagramSegment[];
};

const DIAGRAM_COLORS = {
  delay: '#ef4444',
  ws: '#2563eb',
  buf: '#4b5563',
  env: '#8b5cf6',
  avatar: '#f59e0b',
  broadcast: '#14b8a6',
} as const;

const SVG_NS = 'http://www.w3.org/2000/svg';
const LANE_H = 18;
const LANE_GAP = 10;
const MARGIN_L = 56;
const MARGIN_R = 14;
const RULER_TOP = 6;
const RULER_H = 20;
const RULER_MARGIN_BOTTOM = 8;
const MARGIN_T = RULER_TOP + RULER_H + RULER_MARGIN_BOTTOM;
const MARGIN_B = 6;
const MIN_SEG_W = 3;

let lastTimingDiagram: TimingDiagramMessage | null = null;
let timingDiagramResizeBound = false;

function ce<K extends keyof SVGElementTagNameMap>(
  tag: K,
  attrs: Record<string, string | number>,
): SVGElementTagNameMap[K] {
  const el = document.createElementNS(SVG_NS, tag) as SVGElementTagNameMap[K];
  for (const [k, v] of Object.entries(attrs)) {
    el.setAttribute(k, String(v));
  }
  return el;
}

function laneTotal(segments: DiagramSegment[]): number {
  return segments.reduce((sum, seg) => sum + Math.max(0, seg.ms), 0);
}

function getChannelLaneLabel(): string {
  return t('diagram.laneYou');
}

function buildChannelLane(r: TimingDiagramMessage): DiagramLane {
  const c = Math.round(r.ch_measured_ms);
  const segments =
    r.ch_offset_ms >= 0
      ? [
        { ms: r.ch_total_ms, color: DIAGRAM_COLORS.delay },
        { ms: c, color: DIAGRAM_COLORS.ws },
        { ms: r.buf, color: DIAGRAM_COLORS.buf },
        { ms: r.ch_offset_ms, color: DIAGRAM_COLORS.env },
        { ms: r.A, color: DIAGRAM_COLORS.avatar },
      ]
      : [
        { ms: r.ch_total_ms, color: DIAGRAM_COLORS.delay },
        { ms: c + r.ch_offset_ms, color: DIAGRAM_COLORS.ws },
        { ms: r.buf, color: DIAGRAM_COLORS.buf },
        { ms: r.A, color: DIAGRAM_COLORS.avatar },
      ];
  return {
    label: getChannelLaneLabel(),
    segments,
  };
}

function buildBroadcastLane(r: TimingDiagramMessage): DiagramLane {
  return {
    label: t('diagram.laneBroadcast'),
    segments: [
      { ms: r.master_delay, color: DIAGRAM_COLORS.delay },
      { ms: r.R, color: DIAGRAM_COLORS.broadcast },
    ],
  };
}

function renderNoDataDiagram(lines: string[]): void {
  latencyContent.textContent = '';
  const noData = h('div', { class: 'timing-diagram-no-data' });
  for (const line of lines) {
    noData.appendChild(h('p', null, line));
  }
  latencyContent.appendChild(
    h('div', { class: 'timing-diagram-wrap' }, noData),
  );
}

function renderDiagramLegend(legend: HTMLElement): void {
  legend.textContent = '';
  const row1 = [
    { color: DIAGRAM_COLORS.ws, label: t('diagram.ws') },
    { color: DIAGRAM_COLORS.buf, label: t('diagram.buf') },
    { color: DIAGRAM_COLORS.env, label: t('diagram.env') },
    { color: DIAGRAM_COLORS.avatar, label: t('diagram.avatar') },
    { color: DIAGRAM_COLORS.broadcast, label: t('diagram.broadcast') },
  ];
  for (const item of row1) {
    legend.appendChild(
      h('div', { class: 'legend-item' },
        h('span', { class: 'legend-swatch', style: `background:${item.color}` }),
        item.label,
      ),
    );
  }
  legend.appendChild(h('div', { class: 'legend-break' }));
  legend.appendChild(
    h('div', { class: 'legend-item' },
      h('span', { class: 'legend-swatch', style: `background:${DIAGRAM_COLORS.delay}` }),
      h('strong', null, t('diagram.delay')),
      ` ${t('diagram.delayDesc')}`,
    ),
  );
  legend.appendChild(
    h('div', { class: 'legend-item' },
      h('span', { style: 'color:#ef4444; font-size:12px; margin-right:2px' }, '\u25BC'),
      t('diagram.listenTiming'),
    ),
  );
}

function renderTimingDiagram(r: TimingDiagramMessage): void {
  if (r.ch_measured_ms < 0) {
    renderNoDataDiagram([t('diagram.noDataWs')]);
    return;
  }

  const lanes: DiagramLane[] = [
    buildChannelLane(r),
    buildBroadcastLane(r),
  ];

  latencyContent.textContent = '';
  const wrap = h('div', { class: 'timing-diagram-wrap' });
  const svg = ce('svg', { width: '100%', role: 'img' });
  const legend = h('div', { class: 'legend' });
  wrap.append(svg, legend);
  latencyContent.appendChild(wrap);

  const containerW = Math.max(
    320,
    Math.round(wrap.clientWidth || latencyContent.clientWidth || 640),
  );
  const usableW = Math.max(1, containerW - MARGIN_L - MARGIN_R);
  let maxMs = 0;
  for (const lane of lanes) {
    maxMs = Math.max(maxMs, laneTotal(lane.segments));
  }
  if (maxMs <= 0) {
    renderNoDataDiagram([t('diagram.noData')]);
    return;
  }
  const scale = usableW / maxMs;

  const totalH = MARGIN_T + lanes.length * (LANE_H + LANE_GAP) + MARGIN_B;
  svg.setAttribute('height', String(totalH));
  svg.setAttribute('viewBox', `0 0 ${containerW} ${totalH}`);
  svg.innerHTML = '';

  const baseY = RULER_TOP + RULER_H;
  svg.appendChild(ce('line', {
    x1: MARGIN_L,
    y1: baseY,
    x2: MARGIN_L + maxMs * scale,
    y2: baseY,
    stroke: '#4a5568',
    'stroke-width': 1,
  }));

  const rawStep = maxMs / 8;
  const mag = Math.pow(10, Math.floor(Math.log10(Math.max(rawStep, 1))));
  const niceMul = [1, 2, 5, 10].find((n) => n * mag >= rawStep) ?? 10;
  const step = Math.max(1, Math.round(niceMul * mag));
  const tickH = 5;
  for (let ms = 0; ms <= maxMs; ms += step) {
    const x = MARGIN_L + ms * scale;
    svg.appendChild(ce('line', {
      x1: x,
      y1: baseY - tickH,
      x2: x,
      y2: baseY,
      stroke: '#4a5568',
      'stroke-width': 1,
    }));
    const label = ce('text', {
      x,
      y: baseY - tickH - 3,
      'text-anchor': 'middle',
      'font-size': '9px',
      fill: '#6b7280',
    });
    label.textContent = ms === 0 ? '0 ms' : String(ms);
    svg.appendChild(label);
  }

  lanes.forEach((lane, li) => {
    const y = MARGIN_T + li * (LANE_H + LANE_GAP);
    const totalMs = laneTotal(lane.segments);
    const leftPadMs = maxMs - totalMs;

    const laneLabel = ce('text', {
      x: MARGIN_L - 8,
      y: y + LANE_H / 2 + 4,
      'text-anchor': 'end',
      class: 'lane-label',
    });
    laneLabel.textContent = lane.label;
    svg.appendChild(laneLabel);

    // 累積 ms からセグメント境界を算出し、全レーンの右端を揃える
    let cumMs = 0;
    let x = MARGIN_L + leftPadMs * scale;
    let hearX = -1;
    for (const seg of lane.segments) {
      if (seg.ms <= 0) continue;
      cumMs += seg.ms;
      const nextX = MARGIN_L + (leftPadMs + cumMs) * scale;
      const w = Math.max(MIN_SEG_W, nextX - x);
      svg.appendChild(ce('rect', {
        x,
        y,
        width: w,
        height: LANE_H,
        fill: seg.color,
        stroke: 'rgba(255,255,255,0.08)',
        'stroke-width': 0.5,
      }));
      if (w > 24) {
        const segLabel = ce('text', {
          x: x + w / 2,
          y: y + LANE_H / 2,
          'text-anchor': 'middle',
          'dominant-baseline': 'central',
          class: 'segment-label',
        });
        segLabel.textContent = String(Math.round(seg.ms));
        svg.appendChild(segLabel);
      }
      if (seg.color === DIAGRAM_COLORS.avatar) hearX = x;
      x += w;
    }
    // 環境遅延右端に ▼ マーカー（出演者が音を聴くタイミング）
    if (li === 0 && hearX >= 0) {
      const marker = ce('text', {
        x: hearX,
        y: y + 5,
        'text-anchor': 'middle',
        'font-size': '10px',
        fill: '#ef4444',
      });
      marker.textContent = '\u25BC';
      svg.appendChild(marker);
    }
  });

  renderDiagramLegend(legend);
}

function bindTimingDiagramResize(): void {
  if (timingDiagramResizeBound) return;
  timingDiagramResizeBound = true;
  let rafId = 0;
  window.addEventListener('resize', () => {
    if (!lastTimingDiagram) return;
    cancelAnimationFrame(rafId);
    rafId = requestAnimationFrame(() => {
      if (lastTimingDiagram) renderTimingDiagram(lastTimingDiagram);
    });
  });
}

export function showMeasuring(n: number): void {
  lastTimingDiagram = null;
  const statusMsg = tr(
    'status.measuring',
    { current: n, total: state.pingTotal },
    'レイテンシ計測中... ({{current}}/{{total}})',
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

export function showTimingDiagram(r: TimingDiagramMessage): void {
  bindTimingDiagramResize();
  lastTimingDiagram = r;
  setStatus(t('status.receiving'), 'ok');
  renderTimingDiagram(r);
  latencyCard.classList.add('has-background-info-light');
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
