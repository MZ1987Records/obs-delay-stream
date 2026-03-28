import '@fortawesome/fontawesome-free/css/all.min.css';
import 'bulma/css/bulma.min.css';
import '../css/ui.css';
import {
  applyStaticI18n,
  initI18n,
  renderLanguageSwitcher,
  t,
  tr,
} from './i18n';
import {
  getOptionalElement,
  getRequiredButtonElement,
  getRequiredElement,
  getRequiredInputElement,
  getRequiredSelectElement,
} from './dom';
initI18n();
try {
  const buildTimestamp = (document.documentElement.dataset.buildTimestamp || '').trim();
  if (buildTimestamp && !/^@.+@$/.test(buildTimestamp)) {
    console.info('[obs-delay-stream] receiver build:', buildTimestamp);
  }
} catch { }
applyStaticI18n();
renderLanguageSwitcher();

type JsonRecord = Record<string, unknown>;

type AudioDecoderConfigLike = {
  codec: string;
  sampleRate: number;
  numberOfChannels: number;
};

type EncodedAudioChunkInitLike = {
  type: 'key' | 'delta';
  timestamp: number;
  duration?: number;
  data: BufferSource;
};

type EncodedAudioChunkLike = {};

type EncodedAudioChunkConstructorLike = new (init: EncodedAudioChunkInitLike) => EncodedAudioChunkLike;

type AudioDataLike = {
  numberOfFrames: number;
  numberOfChannels: number;
  sampleRate: number;
  format?: string;
  copyTo: (destination: BufferSource, options: { planeIndex: number }) => void;
  close: () => void;
};

type AudioDecoderLike = {
  configure: (config: AudioDecoderConfigLike) => void;
  decode: (chunk: EncodedAudioChunkLike) => void;
  close: () => void;
};

type AudioDecoderConstructorLike = {
  new (init: { output: (audioData: AudioDataLike) => void; error: (err: unknown) => void }): AudioDecoderLike;
  isConfigSupported: (config: AudioDecoderConfigLike) => Promise<{ supported: boolean }>;
};

type OpusDecodedFrame = {
  channelData: Float32Array[];
  samplesDecoded: number;
  sampleRate?: number;
  errors?: Array<{ code?: number; message?: string }>;
};

type OpusDecoderInstance = {
  decodeFrame: (data: Uint8Array) => OpusDecodedFrame | null;
  reset?: () => Promise<void>;
  free: () => void;
  ready: Promise<void>;
};

type OpusDecoderModule = {
  OpusDecoder: new (opts: { sampleRate: number; channels: number }) => OpusDecoderInstance;
};

type WindowWithWebkitAudioContext = Window & {
  webkitAudioContext?: typeof AudioContext;
};

type WindowWithOpusDecoder = Window & {
  'opus-decoder'?: OpusDecoderModule;
};

function isRecord(value: unknown): value is JsonRecord {
  return typeof value === 'object' && value !== null;
}

function safeParseJson(text: string): unknown {
  try {
    return JSON.parse(text);
  } catch {
    return null;
  }
}

// ============================================================
// URL プレビュー
// ============================================================
function getHostDomain(): string {
  return location.hostname || 'localhost';
}

function buildUrl(ip: string, sid: string, ch: string | number): string {
  // cloudflared/https時はポートなしwss://
  const hasScheme = /^(wss?|https?):\/\//.test(ip);
  const isTunnel = ip.includes('trycloudflare.com')
    || hasScheme || location.protocol === 'https:';
  const cleanIp = ip.replace(/^(wss?|https?):\/\//, '').replace(/\/.*$/, '');
  if (isTunnel) {
    return sid ? `wss://${cleanIp}/${sid}/${ch}` : `wss://${cleanIp}/${t('url.streamIdMissing')}/${ch}`;
  } else {
    return sid ? `ws://${cleanIp}:19000/${sid}/${ch}` : `ws://${cleanIp}:19000/${t('url.streamIdMissing')}/${ch}`;
  }
}

const hostDomain = getHostDomain();
const MIN_CH = 1;
let MAX_CH = 20;
const PING_SAMPLES = 10;
let CH_RANGE_TEXT = t('format.chRange', { min: MIN_CH, max: MAX_CH });

const sidInput = getRequiredInputElement('sidInput');
const chInput = getRequiredInputElement('chInput');
const chHint = getOptionalElement<HTMLElement>('chHint');
const urlPreview = getOptionalElement<HTMLElement>('urlPreview');

function applyChRange(max: number): void {
  MAX_CH = max;
  CH_RANGE_TEXT = t('format.chRange', { min: MIN_CH, max: MAX_CH });
  if (chHint) chHint.textContent = `(${CH_RANGE_TEXT})`;
  chInput.min = String(MIN_CH);
  chInput.max = String(MAX_CH);
  const v = parseInt(chInput.value, 10);
  if (Number.isInteger(v) && v > MAX_CH) chInput.value = String(MAX_CH);
  updateUrlPreview();
}
applyChRange(MAX_CH);

type ShebangParams = { sid: string | null; ch: string | null };

function parseShebangParams(): ShebangParams | null {
  if (!location.hash.startsWith('#!')) return null;
  let raw = location.hash.slice(2);
  if (raw.startsWith('/')) raw = raw.slice(1);
  if (raw.startsWith('?')) raw = raw.slice(1);
  if (!raw) return null;

  const safeDecode = (val: string): string => {
    try { return decodeURIComponent(val); }
    catch { return val; }
  };

  // 新形式: #!/{sid}/{ch}
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

function applyUrlParams(): void {
  const params = getInitialParams();
  if (!params) return;
  const sid = params.sid;
  const ch = params.ch;
  if (sid) sidInput.value = sid;
  if (ch) {
    const chNum = parseInt(ch, 10);
    if (!Number.isNaN(chNum) && chNum >= MIN_CH && chNum <= MAX_CH) {
      chInput.value = String(chNum);
    }
  }
  if (sid && ch) fetchMemoPreview(sid, ch);
}
applyUrlParams();

function isChromeBrowser(): boolean {
  const uaData = (navigator as Navigator & { userAgentData?: { brands?: Array<{ brand: string }> } }).userAgentData;
  if (uaData && Array.isArray(uaData.brands)) {
    return uaData.brands.some((brand) => brand.brand === 'Google Chrome');
  }
  const ua = navigator.userAgent || '';
  const hasChromeToken = /Chrome\/\d+/.test(ua) || /CriOS\/\d+/.test(ua);
  if (!hasChromeToken) return false;
  const nonChromeToken = /(Edg|OPR|Opera|SamsungBrowser|UCBrowser|YaBrowser|Vivaldi)\//.test(ua);
  return !nonChromeToken;
}

const browserWarning = getOptionalElement<HTMLElement>('browserWarning');

function updateBrowserWarning(): void {
  if (!browserWarning) return;
  browserWarning.hidden = isChromeBrowser();
}
updateBrowserWarning();

type ConfigResponse = { active_ch?: number };

function isConfigResponse(value: unknown): value is ConfigResponse {
  return isRecord(value)
    && (value.active_ch === undefined || Number.isInteger(value.active_ch));
}

function loadConfig(): void {
  fetch('/config', { cache: 'no-store' })
    .then(r => r.ok ? r.json() : null)
    .then(cfg => {
      if (!cfg || !isConfigResponse(cfg)) return;
      const v = cfg.active_ch;
      if (typeof v === 'number' && Number.isInteger(v) && v >= MIN_CH) applyChRange(v);
    })
    .catch(() => {});
}
loadConfig();

type MemoResponse = { memo?: string };

function isMemoResponse(value: unknown): value is MemoResponse {
  return isRecord(value) && (value.memo === undefined || typeof value.memo === 'string');
}

function fetchMemoPreview(sid: string, ch: string): void {
  const chNum = parseInt(ch, 10);
  if (!sid || !Number.isInteger(chNum) || chNum < MIN_CH || chNum > MAX_CH) return;
  const memoEl = getOptionalElement<HTMLElement>('infoMemo');
  if (!memoEl) return;
  const url = `/memo?sid=${encodeURIComponent(sid)}&ch=${encodeURIComponent(String(chNum))}`;
  fetch(url, { cache: 'no-store' })
    .then(r => r.ok ? r.json() : null)
    .then(data => {
      if (!data || !isMemoResponse(data) || typeof data.memo !== 'string') return;
      updateMemoDisplay(memoEl, data.memo);
    })
    .catch(() => {});
}

function updateUrlPreview(): void {
  const sid = sidInput.value.trim();
  const ch = chInput.value || '1';
  if (urlPreview) urlPreview.textContent = buildUrl(hostDomain, sid, ch);
}

function updateShebang(sid: string, ch: string): void {
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
// AudioContext
// ============================================================
let ws: WebSocket | null = null;
let actx: AudioContext | null = null;
let gainNode: GainNode | null = null;
let nextTime = 0;
const AHEAD = 0.12;
const MAGIC_AUDI = 0x41554449;
const MAGIC_OPUS = 0x4f505553;
let muted = false;
let connecting = false;
let lastWsError = false;
let connectTimer: ReturnType<typeof setTimeout> | null = null;
const CONNECT_TIMEOUT_MS = 7000;
let syncAvailable = false;

// 制御メッセージ / 再同期の状態
let pingCount = 0;
let syncCountdown = 0;
let syncTickTimer: ReturnType<typeof setInterval> | null = null;
let currentSyncInterval = 0;  // 0=オフ
let pendingSyncInterval = 0;

// WebCodecs (Opus)
type OpusPacket = { buf: ArrayBuffer; sampleRate: number; channels: number; frameCount: number };

let opusDecoder: AudioDecoderLike | null = null;
let opusConfig: AudioDecoderConfigLike | null = null;
let opusInitPromise: Promise<boolean> | null = null;
let opusQueue: OpusPacket[] = [];
let opusTsUs = 0;
let opusErrored = false;
let opusErrorReported = false;
let opusMode: 'auto' | 'webcodecs' | 'wasm' = 'auto';
let opusWebCodecsState: 'idle' | 'pending' | 'ready' | 'failed' = 'idle';
let opusWasmState: 'idle' | 'pending' | 'ready' | 'failed' = 'idle';
let opusWasmDecoder: OpusDecoderInstance | null = null;
let opusWasmConfig: { sampleRate: number; numberOfChannels: number } | null = null;
let opusWasmInitPromise: Promise<unknown> | null = null;
let opusWasmLibPromise: Promise<unknown> | null = null;
let opusWasmErrorStreak = 0;
let opusWasmLastErrorTs = 0;
let pcmFallbackRequested = false;
let pcmFallbackReason = '';

function reportOpusError(msg: string): void {
  if (opusErrorReported) return;
  opusErrorReported = true;
  setStatus(msg, 'err');
  try { console.warn('[Opus]', msg); } catch { }
}

function sendPcmFallbackIfPossible(): void {
  if (!pcmFallbackRequested) return;
  if (ws && ws.readyState === WebSocket.OPEN) {
    const payload: { type: string; mode: string; reason?: string } = { type: 'audio_codec', mode: 'pcm' };
    if (pcmFallbackReason) payload.reason = pcmFallbackReason;
    try { ws.send(JSON.stringify(payload)); } catch { }
  }
}

function requestPcmFallback(reason?: string): void {
  if (pcmFallbackRequested) return;
  pcmFallbackRequested = true;
  pcmFallbackReason = reason || '';
  sendPcmFallbackIfPossible();
}

function disableOpus(reason: string, msg?: string): void {
  if (opusErrored) return;
  opusErrored = true;
  requestPcmFallback(reason);
  reportOpusError(msg || t('codec.opusDecodeFailed'));
  opusQueue = [];
}

function markWebCodecsFailed(reason?: string): void {
  if (opusWebCodecsState === 'failed') return;
  opusWebCodecsState = 'failed';
  if (reason) {
    try { console.warn('[Opus][WebCodecs]', reason); } catch { }
  }
}

function markWasmFailed(reason?: string): void {
  if (opusWasmState === 'failed') return;
  opusWasmState = 'failed';
  if (reason) {
    try { console.warn('[Opus][WASM]', reason); } catch { }
  }
}

function noteWasmError(): number {
  const now = Date.now();
  if (now - opusWasmLastErrorTs > 2000) opusWasmErrorStreak = 0;
  opusWasmLastErrorTs = now;
  opusWasmErrorStreak++;
  return opusWasmErrorStreak;
}

function clearWasmErrors(): void {
  opusWasmErrorStreak = 0;
  opusWasmLastErrorTs = 0;
}

function loadOpusWasmLibrary(): Promise<unknown> {
  if (opusWasmLibPromise) return opusWasmLibPromise;
  opusWasmLibPromise = new Promise((resolve, reject) => {
    const s = document.createElement('script');
    s.async = true;
    s.src = 'opus-decoder/opus-decoder.min.js';
    s.onload = () => resolve(true);
    s.onerror = () => reject(new Error('opus-decoder load failed'));
    document.head.appendChild(s);
  });
  return opusWasmLibPromise;
}

// VUメーター
const NUM_BARS = 20;
const meterEl = getRequiredElement<HTMLDivElement>('meter');
const bars: HTMLDivElement[] = [];
for (let i = 0; i < NUM_BARS; i++) {
  const b = document.createElement('div');
  b.className = 'bar';
  b.style.height = '2px';
  meterEl.appendChild(b);
  bars.push(b);
}

// ============================================================
// 音量
// ============================================================
const volSlider = getRequiredInputElement('volSlider');
const dbDisplay = getRequiredElement<HTMLElement>('dbDisplay');
const muteBtn = getRequiredButtonElement('muteBtn');
const muteIcon = getOptionalElement<HTMLElement>('muteIcon');
const syncIntervalSelect = getRequiredSelectElement('syncIntervalSelect');

function sliderToGain(val: number): number {
  // 0→-∞dB, 50→-20dB, 84→-3.0dB, 100→0dB (二乗カーブ)
  if (val <= 0) return 0;
  return Math.pow(val / 100, 2);
}
function gainToDb(gain: number): number {
  if (gain <= 0) return -Infinity;
  return 20 * Math.log10(gain);
}
function formatDb(gain: number): string {
  if (gain <= 0) return '−∞ dB';
  const db = gainToDb(gain);
  return (db >= 0 ? '+' : '') + db.toFixed(1) + ' dB';
}

function onVolumeChange(val: number | string): void {
  const v = typeof val === 'number' ? val : parseInt(val, 10);
  const safeV = Number.isFinite(v) ? v : 0;
  localStorage.setItem('volume', String(safeV));
  const gain = sliderToGain(safeV);
  if (gainNode && !muted) gainNode.gain.value = gain;
  volSlider.disabled = muted;
  volSlider.classList.toggle('is-primary', !muted);
  dbDisplay.textContent = muted ? t('audio.muted') : formatDb(gain);
  const dbClasses = ['tag', 'is-medium', 'has-background-dark', 'has-text-white', 'is-family-monospace'];
  if (muted) {
    dbClasses.push('has-text-grey');
  }
  dbDisplay.className = dbClasses.join(' ');
}

function toggleMute(): void {
  muted = !muted;
  muteBtn.className = muted ? 'button is-ghost has-text-grey' : 'button is-primary';
  volSlider.disabled = muted;
  volSlider.classList.toggle('is-primary', !muted);
  if (muteIcon) muteIcon.className = muted ? 'fas fa-volume-mute' : 'fas fa-volume-up';
  if (gainNode) {
    gainNode.gain.value = muted ? 0
      : sliderToGain(parseInt(volSlider.value, 10));
  }
  const val = parseInt(volSlider.value, 10);
  const gain = sliderToGain(val);
  dbDisplay.textContent = muted ? t('audio.muted') : formatDb(gain);
  const dbClasses = ['tag', 'is-medium', 'has-background-dark', 'has-text-white', 'is-family-monospace'];
  if (muted) {
    dbClasses.push('has-text-grey');
  }
  dbDisplay.className = dbClasses.join(' ');
}

// 初期表示（localStorageから復元）
{
  const savedVol = localStorage.getItem('volume');
  const initVol = savedVol !== null ? Number(savedVol) : 84;
  volSlider.value = String(initVol);
  onVolumeChange(initVol);

  const savedSync = localStorage.getItem('syncInterval');
  if (savedSync !== null) {
    syncIntervalSelect.value = savedSync;
    onSyncIntervalChange(savedSync);
  }
}

// ============================================================
// 接続
// ============================================================
const statusBar = getRequiredElement<HTMLElement>('statusBar');
const statusText = getOptionalElement<HTMLElement>('statusText');
const statusIcon = getOptionalElement<HTMLElement>('statusIcon');
const infoCodec = getRequiredElement<HTMLElement>('infoCodec');
const connectBtn = getRequiredButtonElement('connectBtn');
const stopBtn = getRequiredButtonElement('stopBtn');
const syncBtn = getRequiredButtonElement('syncBtn');
const latencyCard = getRequiredElement<HTMLElement>('latencyCard');
const infoBuf = getRequiredElement<HTMLElement>('infoBuf');
const infoSR = getRequiredElement<HTMLElement>('infoSR');
const infoCH = getRequiredElement<HTMLElement>('infoCH');
const latencyContent = getRequiredElement<HTMLElement>('latencyContent');
const syncBtnLabel = getRequiredElement<HTMLElement>('syncBtnLabel');

type StatusClass = '' | 'ok' | 'err' | 'mea';

function setStatus(msg: string, cls: StatusClass = ''): void {
  if (statusText) statusText.textContent = msg;
  const classes = ['notification'];
  if (cls === 'ok') classes.push('is-success');
  else if (cls === 'err') classes.push('is-danger');
  else if (cls === 'mea') classes.push('is-warning');
  else classes.push('is-dark');
  statusBar.className = classes.join(' ');
  let icon = 'fa-info-circle';
  if (cls === 'ok') icon = 'fa-check-circle';
  else if (cls === 'err') icon = 'fa-times-circle';
  else if (cls === 'mea') icon = 'fa-exclamation-triangle';
  if (statusIcon) statusIcon.className = `fas ${icon}`;
}

function setCodecLabel(text?: string): void {
  infoCodec.textContent = text || '—';
}

function clearConnectTimer(): void {
  if (connectTimer) { clearTimeout(connectTimer); connectTimer = null; }
}

function setInputsEnabled(enabled: boolean): void {
  sidInput.disabled = !enabled;
  chInput.disabled = !enabled;
}

function setDisconnectedUi(): void {
  connectBtn.disabled = false;
  stopBtn.disabled = true;
  syncBtn.disabled = true;
  enableSyncOptions(false);
  latencyCard.classList.remove('has-background-info-light');
  setInputsEnabled(true);
  setCodecLabel('—');
}

function connect(): void {
  if (connecting) return;
  if (ws) ws.close();
  if (!actx) {
    const AudioContextCtor = window.AudioContext || (window as WindowWithWebkitAudioContext).webkitAudioContext;
    if (!AudioContextCtor) {
      setStatus(t('status.connectFailed'), 'err');
      return;
    }
    actx = new AudioContextCtor();
    gainNode = actx.createGain();
    gainNode.gain.value = sliderToGain(parseInt(volSlider.value, 10));
    gainNode.connect(actx.destination);
  }
  nextTime = actx.currentTime + AHEAD;

  const sid = sidInput.value.trim();
  const ch = parseInt(chInput.value, 10);

  if (!sid) { setStatus(t('status.enterStreamId'), 'err'); return; }
  if (!/^[a-z0-9]+$/i.test(sid)) { setStatus(t('status.invalidStreamId'), 'err'); return; }
  // if (!Number.isInteger(ch) || ch < MIN_CH || ch > MAX_CH) {
  //   setStatus(`CH番号は${CH_RANGE_TEXT}で入力してください`, 'err');
  //   return;
  // }

  const url = buildUrl(hostDomain, sid, ch);
  setStatus(t('status.connecting'), '');
  connecting = true;
  lastWsError = false;
  setInputsEnabled(false);
  connectBtn.disabled = true;
  try {
    ws = new WebSocket(url);
  } catch {
    connecting = false;
    setStatus(t('status.connectFailed'), 'err');
    connectBtn.disabled = false;
    setInputsEnabled(true);
    return;
  }
  ws.binaryType = 'arraybuffer';
  const wsLocal = ws;

  clearConnectTimer();
  connectTimer = setTimeout(() => {
    if (ws !== wsLocal) return;
    if (wsLocal.readyState !== WebSocket.OPEN) {
      lastWsError = true;
      setStatus(t('status.connectTimeout'), 'err');
      try { wsLocal.close(); } catch { }
    }
  }, CONNECT_TIMEOUT_MS);

  wsLocal.onopen = () => {
    if (ws !== wsLocal) return;
    clearConnectTimer();
    connecting = false;
    setStatus(t('status.receivingWithUrl', { url }), 'ok');
    connectBtn.disabled = true;
    stopBtn.disabled = false;
    syncBtn.disabled = false;
    enableSyncOptions(true);
    latencyCard.classList.add('has-background-info-light');
    sendPcmFallbackIfPossible();
  };
  wsLocal.onerror = () => {
    if (ws !== wsLocal) return;
    lastWsError = true;
    setStatus(t('status.connectionError'), 'err');
  };
  wsLocal.onclose = (ev) => {
    if (ws !== wsLocal) return;
    clearConnectTimer();
    connecting = false;
    if (ev && ev.code === 1008 && ev.reason === 'stream_id_mismatch') {
      lastWsError = true;
      setStatus(t('status.streamIdMismatch'), 'err');
    } else if (ev && ev.code === 1008 && ev.reason === 'ch_out_of_range') {
      lastWsError = true;
      setStatus(t('status.chOutOfRange', { range: CH_RANGE_TEXT }), 'err');
    } else if (!lastWsError) {
      setStatus(t('status.disconnected'), '');
    }
    lastWsError = false;
    setDisconnectedUi();
    ws = null;
  };
  wsLocal.onmessage = (ev: MessageEvent) => {
    if (ws !== wsLocal) return;
    if (ev.data instanceof ArrayBuffer) handleAudio(ev.data);
    else if (typeof ev.data === 'string') handleControl(ev.data);
  };
}

function disconnect(): void {
  stopAutoSync();
  clearConnectTimer();
  connecting = false;
  syncBtn.disabled = true;
  enableSyncOptions(false);
  const wsLocal = ws;
  ws = null;
  setDisconnectedUi();
  setStatus(t('status.disconnectedByUser'), '');
  if (wsLocal) {
    try { wsLocal.close(); } catch { }
  }
}

// ============================================================
// 音声受信
// ============================================================
function handleAudio(buf: ArrayBuffer): void {
  if (buf.byteLength < 16) return;
  const u32 = new Uint32Array(buf, 0, 4);
  const magic = u32[0];
  const sampleRate = u32[1], channels = u32[2], frameCount = u32[3];

  if (magic === MAGIC_AUDI) {
    handlePcm16(buf, sampleRate, channels, frameCount);
  } else if (magic === MAGIC_OPUS) {
    handleOpus(buf, sampleRate, channels, frameCount);
  }
}

function playBuffer(abuf: AudioBuffer | null): void {
  if (!abuf || !actx || !gainNode) return;
  const src = actx.createBufferSource();
  src.buffer = abuf;
  src.connect(gainNode);

  const now = actx.currentTime;
  if (nextTime < now + 0.01) nextTime = now + AHEAD;
  src.start(nextTime);
  nextTime += abuf.duration;

  const bufMs = Math.round((nextTime - actx.currentTime) * 1000);
  infoBuf.textContent = bufMs + ' ms';
  updateMeter(abuf.getChannelData(0));
}

function handlePcm16(buf: ArrayBuffer, sampleRate: number, channels: number, frameCount: number): void {
  if (!actx) return;
  infoSR.textContent = sampleRate + ' Hz';
  infoCH.textContent = channels + 'ch';
  setCodecLabel('PCM16');

  const pcm = new Int16Array(buf, 16);
  const abuf = actx.createBuffer(channels, frameCount, sampleRate);
  for (let c = 0; c < channels; c++) {
    const d = abuf.getChannelData(c);
    for (let f = 0; f < frameCount; f++) {
      d[f] = pcm[f * channels + c] / 32768;
    }
  }
  playBuffer(abuf);
}

function handleOpus(buf: ArrayBuffer, sampleRate: number, channels: number, frameCount: number): void {
  if (opusErrored) return;
  if (opusMode === 'webcodecs') {
    if (ensureWebCodecsDecoder(sampleRate, channels)) {
      decodeOpusPacketWebCodecs(buf, sampleRate, channels, frameCount);
      return;
    }
    if (opusWebCodecsState === 'failed') {
      opusMode = 'auto';
    } else {
      opusQueue.push({ buf, sampleRate, channels, frameCount });
      return;
    }
  }
  if (opusMode === 'wasm') {
    if (ensureWasmDecoder(sampleRate, channels)) {
      decodeOpusPacketWasm(buf, sampleRate, channels, frameCount);
      return;
    }
    if (opusWasmState === 'failed') {
      opusMode = 'auto';
    } else {
      opusQueue.push({ buf, sampleRate, channels, frameCount });
      return;
    }
  }

  // auto: WebCodecs優先、失敗時WASM
  if (ensureWebCodecsDecoder(sampleRate, channels)) {
    opusMode = 'webcodecs';
    decodeOpusPacketWebCodecs(buf, sampleRate, channels, frameCount);
    return;
  }
  if (opusWebCodecsState === 'pending') {
    opusQueue.push({ buf, sampleRate, channels, frameCount });
    return;
  }
  if (ensureWasmDecoder(sampleRate, channels)) {
    opusMode = 'wasm';
    decodeOpusPacketWasm(buf, sampleRate, channels, frameCount);
    return;
  }
  if (opusWasmState === 'pending') {
    opusQueue.push({ buf, sampleRate, channels, frameCount });
    return;
  }

  disableOpus('opus_unavailable', t('codec.opusUnavailable'));
}

function ensureWebCodecsDecoder(sampleRate: number, channels: number): boolean {
  if (opusWebCodecsState === 'failed') return false;
  if (opusDecoder &&
    opusConfig &&
    opusConfig.sampleRate === sampleRate &&
    opusConfig.numberOfChannels === channels) {
    opusWebCodecsState = 'ready';
    return true;
  }
  if (opusInitPromise) return false;
  if (opusDecoder) {
    try { opusDecoder.close(); } catch { }
  }
  opusDecoder = null;
  opusConfig = null;
  opusTsUs = 0;

  if (!window.isSecureContext) {
    markWebCodecsFailed('insecure_context');
    return false;
  }
  const AudioDecoderCtor = (window as unknown as { AudioDecoder?: AudioDecoderConstructorLike }).AudioDecoder;
  if (!AudioDecoderCtor) {
    markWebCodecsFailed('no_audio_decoder');
    return false;
  }

  const config: AudioDecoderConfigLike = { codec: 'opus', sampleRate, numberOfChannels: channels };
  opusWebCodecsState = 'pending';
  opusInitPromise = AudioDecoderCtor.isConfigSupported(config).then(s => {
    if (!s.supported) {
      markWebCodecsFailed('config_unsupported');
      return false;
    }
    opusDecoder = new AudioDecoderCtor({
      output: onOpusDecoded,
      error: () => {
        markWebCodecsFailed('decode_error');
        opusMode = 'auto';
      }
    });
    opusDecoder.configure(config);
    opusConfig = config;
    opusTsUs = 0;
    opusWebCodecsState = 'ready';
    return true;
  }).then(ok => {
    opusInitPromise = null;
    if (ok) {
      opusMode = 'webcodecs';
      flushOpusQueue();
      return true;
    }
    return false;
  }).catch(() => {
    opusInitPromise = null;
    markWebCodecsFailed('init_failed');
    return false;
  });
  return false;
}

function ensureWasmDecoder(sampleRate: number, channels: number): boolean {
  if (opusWasmState === 'failed') return false;
  if (opusWasmDecoder &&
    opusWasmConfig &&
    opusWasmConfig.sampleRate === sampleRate &&
    opusWasmConfig.numberOfChannels === channels) {
    opusWasmState = 'ready';
    return true;
  }
  if (opusWasmInitPromise) return false;
  if (opusWasmDecoder) {
    try { opusWasmDecoder.free(); } catch { }
  }
  opusWasmDecoder = null;
  opusWasmConfig = null;

  opusWasmState = 'pending';
  opusWasmInitPromise = loadOpusWasmLibrary().then(() => {
    const mod = (window as WindowWithOpusDecoder)['opus-decoder'];
    if (!mod || !mod.OpusDecoder) throw new Error('OpusDecoder missing');
    opusWasmDecoder = new mod.OpusDecoder({ sampleRate, channels });
    opusWasmConfig = { sampleRate, numberOfChannels: channels };
    return opusWasmDecoder.ready;
  }).then(() => {
    opusWasmInitPromise = null;
    opusWasmState = 'ready';
    opusMode = 'wasm';
    flushOpusQueue();
    return true;
  }).catch((e) => {
    opusWasmInitPromise = null;
    markWasmFailed(e && e.message ? e.message : 'init_failed');
    if (opusWebCodecsState === 'failed') {
      disableOpus('opus_unavailable', t('codec.opusUnavailable'));
    }
    return false;
  });
  return false;
}

function flushOpusQueue(): void {
  if (opusErrored) { opusQueue = []; return; }
  const q = opusQueue;
  opusQueue = [];
  for (const pkt of q) {
    if (opusMode === 'webcodecs') {
      decodeOpusPacketWebCodecs(pkt.buf, pkt.sampleRate, pkt.channels, pkt.frameCount);
    } else if (opusMode === 'wasm') {
      decodeOpusPacketWasm(pkt.buf, pkt.sampleRate, pkt.channels, pkt.frameCount);
    } else {
      handleOpus(pkt.buf, pkt.sampleRate, pkt.channels, pkt.frameCount);
    }
  }
}

function decodeOpusPacketWebCodecs(
  buf: ArrayBuffer,
  sampleRate: number,
  channels: number,
  frameCount: number
): void {
  if (!opusDecoder) return;
  infoSR.textContent = sampleRate + ' Hz';
  infoCH.textContent = channels + 'ch';

  const EncodedAudioChunkCtor = (window as unknown as { EncodedAudioChunk?: EncodedAudioChunkConstructorLike })
    .EncodedAudioChunk;
  if (!EncodedAudioChunkCtor) {
    markWebCodecsFailed('no_encoded_audio_chunk');
    return;
  }

  const payload = new Uint8Array(buf, 16);
  const durationUs = frameCount > 0 ? Math.round(frameCount * 1000000 / sampleRate) : 0;
  const chunk = new EncodedAudioChunkCtor({
    type: 'key',
    timestamp: opusTsUs,
    duration: durationUs,
    data: payload
  });
  opusDecoder.decode(chunk);
  if (durationUs > 0) opusTsUs += durationUs;
}

function onOpusDecoded(ad: AudioDataLike): void {
  const abuf = audioDataToBuffer(ad);
  if (abuf) {
    setCodecLabel('Opus (WebCodecs)');
    playBuffer(abuf);
  }
  ad.close();
}

function audioDataToBuffer(ad: AudioDataLike): AudioBuffer | null {
  if (!actx) return null;
  const frames = ad.numberOfFrames;
  const channels = ad.numberOfChannels;
  const sampleRate = ad.sampleRate;
  const fmt = ad.format || '';
  const planar = fmt.endsWith('-planar');
  const f32 = fmt.startsWith('f32');
  const s16 = fmt.startsWith('s16');
  if (!f32 && !s16) {
    markWebCodecsFailed('unsupported_audio_format');
    return null;
  }

  const abuf = actx.createBuffer(channels, frames, sampleRate);
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
        for (let i = 0; i < frames; i++) out[i] = tmp[i * channels + c] / 32768;
      }
    }
  }
  return abuf;
}

function decodeOpusPacketWasm(
  buf: ArrayBuffer,
  sampleRate: number,
  channels: number,
  frameCount: number
): void {
  if (!opusWasmDecoder || !actx) return;
  infoSR.textContent = sampleRate + ' Hz';
  infoCH.textContent = channels + 'ch';

  const payload = new Uint8Array(buf, 16);
  let decoded: OpusDecodedFrame | null;
  try {
    decoded = opusWasmDecoder.decodeFrame(payload);
  } catch (e: unknown) {
    const streak = noteWasmError();
    try { console.warn('[Opus][WASM] decode_error'); } catch { }
    if (streak >= 3) {
      markWasmFailed('decode_error');
      if (opusWebCodecsState === 'failed') {
        disableOpus('opus_unavailable', t('codec.opusDecodeFailed'));
      } else {
        opusMode = 'auto';
      }
    } else if (!opusWasmInitPromise && opusWasmDecoder && typeof opusWasmDecoder.reset === 'function') {
      opusWasmState = 'pending';
      opusWasmInitPromise = opusWasmDecoder.reset().then(() => {
        opusWasmInitPromise = null;
        opusWasmState = 'ready';
      }).catch(() => {
        opusWasmInitPromise = null;
        markWasmFailed('reset_failed');
      });
    }
    return;
  }
  if (!decoded || !decoded.channelData || !decoded.samplesDecoded) return;
  if (decoded.errors && decoded.errors.length) {
    try { console.warn('[Opus][WASM] decode errors', decoded.errors[0]); } catch { }
  }
  clearWasmErrors();

  const outRate = decoded.sampleRate || sampleRate;
  const outCh = decoded.channelData.length || channels;
  const abuf = actx.createBuffer(outCh, decoded.samplesDecoded, outRate);
  for (let c = 0; c < outCh; c++) {
    const src = decoded.channelData[c];
    const out = abuf.getChannelData(c);
    if (src.length > decoded.samplesDecoded) out.set(src.subarray(0, decoded.samplesDecoded));
    else out.set(src);
  }
  setCodecLabel('Opus (WASM)');
  playBuffer(abuf);
}

// ============================================================
// 制御メッセージ
// ============================================================
type LatencyResultMessage = {
  one_way: number;
  avg_rtt: number;
  min: number;
  max: number;
};

function isLatencyResultMessage(value: JsonRecord): value is LatencyResultMessage {
  return typeof value.one_way === 'number'
    && typeof value.avg_rtt === 'number'
    && typeof value.min === 'number'
    && typeof value.max === 'number';
}

function handleControl(text: string): void {
  const msg = safeParseJson(text);
  if (!isRecord(msg) || typeof msg.type !== 'string') return;

  switch (msg.type) {
    case 'session_info':
      // サーバーからセッション確認情報を受信
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
      if (ws && ws.readyState === WebSocket.OPEN) {
        const payload: { type: string; seq?: number } = { type: 'pong' };
        if (typeof msg.seq === 'number') payload.seq = msg.seq;
        ws.send(JSON.stringify(payload));
      }
      showMeasuring(++pingCount);
      break;

    case 'latency_result':
      if (isLatencyResultMessage(msg)) showLatencyResult(msg);
      pingCount = 0;
      break;

    case 'apply_delay':
      {
        const reason = (typeof msg.reason === 'string') ? msg.reason : '';
        if (reason === 'manual_adjust') break;
        if (reason === 'auto_measure') {
          if (typeof msg.ms === 'number' || typeof msg.ms === 'string') showApplied(msg.ms);
          break;
        }
        // 旧サーバ互換: reason未送信時は「計測→適用待ち」の文脈だけ表示
        if (getOptionalElement<HTMLElement>('waitingApply')) {
          if (typeof msg.ms === 'number' || typeof msg.ms === 'string') showApplied(msg.ms);
        }
      }
      break;
  }
}

function updateMemoDisplay(memoEl: HTMLElement | null, memoText: unknown): void {
  if (!memoEl) return;
  const text = (typeof memoText === 'string') ? memoText.trim() : '';
  if (!text.length) {
    memoEl.textContent = '';
    memoEl.hidden = true;
    return;
  }
  memoEl.textContent = text;
  memoEl.hidden = false;
}

// ============================================================
// 再同期
// ============================================================
function resync(): void {
  if (!actx) return;
  // バッファをリセットして次のパケットから即座に再開
  nextTime = actx.currentTime + AHEAD;
  setStatus(t('status.resynced'), 'ok');
  // 0.5秒後に通常表示に戻す
  setTimeout(() => {
    if (ws && ws.readyState === WebSocket.OPEN)
      setStatus(t('status.receiving'), 'ok');
  }, 500);
}

function startAutoSync(interval: number): void {
  stopAutoSync();
  if (interval <= 0) return;
  currentSyncInterval = interval;
  syncCountdown = interval;
  updateCountdown();
  syncTickTimer = setInterval(() => {
    syncCountdown--;
    updateCountdown();
    if (syncCountdown <= 0) {
      resync();
      syncCountdown = currentSyncInterval;
    }
  }, 1000);
}

function stopAutoSync(): void {
  if (syncTickTimer) { clearInterval(syncTickTimer); syncTickTimer = null; }
  currentSyncInterval = 0;
  updateCountdown();
}

function onSyncIntervalChange(interval: number | string): void {
  const value = Number(interval);
  localStorage.setItem('syncInterval', String(value));
  pendingSyncInterval = Number.isFinite(value) ? value : 0;
  if (pendingSyncInterval === 0) {
    stopAutoSync();
    return;
  }
  if (syncAvailable) {
    startAutoSync(pendingSyncInterval);
  }
}

function enableSyncOptions(enabled: boolean): void {
  syncAvailable = enabled;
  if (!enabled) {
    stopAutoSync();
    return;
  }
  onSyncIntervalChange(syncIntervalSelect.value);
}

function updateCountdown(): void {
  if (syncTickTimer && currentSyncInterval > 0) {
    syncBtnLabel.textContent = tr(
      'button.resyncCountdown',
      { sec: syncCountdown },
      '再同期（あと{{sec}}秒）',
      'Resync ({{sec}}s)'
    );
  } else {
    syncBtnLabel.textContent = tr('button.resync', {}, '再同期', 'Resync');
  }
}

// ============================================================
// 計測UI
// ============================================================
function showMeasuring(n: number): void {
  const statusText = tr(
    'status.measuring',
    { current: n, total: PING_SAMPLES },
    '遅延計測中... ({{current}}/{{total}})',
    'Measuring latency... ({{current}}/{{total}})'
  );
  const measuringText = tr(
    'latency.measuring',
    { current: n, total: PING_SAMPLES },
    '計測中 ({{current}} / {{total}} ping)',
    'Measuring ({{current}} / {{total}} ping)'
  );
  setStatus(statusText, 'mea');
  latencyContent.innerHTML = `
<div class="measuring-box">
  ${measuringText}<span class="dots"><span>.</span><span>.</span><span>.</span></span>
  <progress class="progress is-small is-warning" style="margin-top:6px" value="${n}" max="${PING_SAMPLES}">${n}</progress>
</div>`;
}

function showLatencyResult(r: LatencyResultMessage): void {
  setStatus(t('status.receiving'), 'ok');
  const colCls = r.avg_rtt < 50 ? 'has-text-success' : r.avg_rtt < 150 ? 'has-text-warning' : 'has-text-danger';
  const lbl = r.avg_rtt < 50
    ? tr('quality.good', {}, '良好', 'Good')
    : r.avg_rtt < 150
      ? tr('quality.normal', {}, '普通', 'Normal')
      : tr('quality.high', {}, '高遅延', 'High latency');
  const estimatedOneWay = tr('latency.estimatedOneWay', {}, '推定片道遅延 (RTT÷2)', 'Estimated one-way latency (RTT/2)');
  const avgRoundTrip = tr('latency.averageRoundTrip', { label: lbl }, '平均往復遅延 ({{label}})', 'Average round-trip latency ({{label}})');
  const minRtt = tr('latency.minRtt', {}, '最小RTT', 'Min RTT');
  const maxRtt = tr('latency.maxRtt', {}, '最大RTT', 'Max RTT');
  const waitingApply = tr(
    'latency.waitingApply',
    {},
    'OBSが遅延設定を反映するまでお待ちください。',
    'Please wait while OBS applies delay settings.'
  );
  latencyContent.innerHTML = `
<div class="latency-grid">
  <div class="metric has-text-centered">
    <span class="val ${colCls}">${r.one_way.toFixed(1)}</span>
    <span class="unit has-text-grey">ms</span>
    <div class="lbl has-text-grey">${estimatedOneWay}</div>
  </div>
  <div class="metric has-text-centered">
    <span class="val ${colCls}">${r.avg_rtt.toFixed(1)}</span>
    <span class="unit has-text-grey">ms RTT</span>
    <div class="lbl has-text-grey">${avgRoundTrip}</div>
  </div>
  <div class="metric has-text-centered">
    <span class="val" style="color:#888">${r.min.toFixed(1)}</span>
    <span class="unit has-text-grey">ms</span>
    <div class="lbl has-text-grey">${minRtt}</div>
  </div>
  <div class="metric has-text-centered">
    <span class="val" style="color:#888">${r.max.toFixed(1)}</span>
    <span class="unit has-text-grey">ms</span>
    <div class="lbl has-text-grey">${maxRtt}</div>
  </div>
</div>
<div class="notification is-info py-3 px-4 mt-3" id="waitingApply">
  <i class="fas fa-info-circle mr-1"></i>
  ${waitingApply}
</div>`;
  latencyCard.classList.add('has-background-info-light');
}

function showApplied(ms: number | string): void {
  document.querySelectorAll('#waitingApply').forEach((el) => el.remove());
  document.querySelectorAll('#appliedDelayNote').forEach((el) => el.remove());
  const appliedText = tr(
    'latency.applied',
    { ms: Number.parseFloat(String(ms)).toFixed(1) },
    'OBSがサブch遅延を <strong>{{ms}} ms</strong> に自動設定しました',
    'OBS auto-set sub-channel delay to <strong>{{ms}} ms</strong>'
  );
  latencyContent.innerHTML +=
    `<div class="notification is-success py-3 px-4 mt-3" id="appliedDelayNote">
  <i class="fas fa-check-circle mr-1"></i>
  ${appliedText}
</div>`;
}

// ============================================================
// VUメーター
// ============================================================
function updateMeter(samples: Float32Array): void {
  const g = gainNode ? gainNode.gain.value : 1;
  const step = Math.floor(samples.length / NUM_BARS) || 1;
  for (let i = 0; i < NUM_BARS; i++) {
    let rms = 0;
    for (let j = 0; j < step; j++) { const s = (samples[i * step + j] || 0) * g; rms += s * s; }
    rms = Math.sqrt(rms / step);
    const h = Math.min(100, Math.round(rms * 500));
    bars[i].style.height = Math.max(2, h) + '%';
    bars[i].style.background = muted ? '#333'
      : h > 80 ? '#ef4444' : h > 50 ? '#f59e0b' : '#22c55e';
  }
}

Object.assign(window, {
  connect,
  disconnect,
  resync,
  onSyncIntervalChange,
  toggleMute,
  onVolumeChange,
});
