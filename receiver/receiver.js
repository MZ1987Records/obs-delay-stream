const I18N_RESOURCES = {
  ja: {
    translation: {
      ui: {
        browserWarning: 'このページはChrome以外のブラウザでは正常に動作しない場合があります。Chromeの利用を推奨します。',
        connectionSettings: '接続設定',
        connectionSettingsNote: '（通常は変更不要）',
        streamId: '配信ID',
        streamIdHint: '(半角英数字)',
        streamIdPlaceholder: 'myshow2024',
        chNumber: 'CH番号',
        resync: '再同期',
        autoResyncInterval: '自動再同期間隔',
        off: 'オフ',
        volumeControl: '音量コントロール',
        sampleRate: 'SR',
        channel: 'CH',
        buffer: 'Buffer',
        codec: 'Codec',
        latencyMeasure: '遅延計測'
      },
      button: {
        connect: '接続',
        disconnect: '切断',
        resync: '再同期',
        resyncCountdown: '再同期（あと{{sec}}秒）'
      },
      audio: {
        mute: 'ミュート',
        muted: 'ミュート',
        volume: '音量'
      },
      help: {
        chrome: 'Chrome推奨。初回接続時に音声再生の許可を求められたら「許可」してください。',
        lan: 'LAN内(IPアドレス直接)の場合: OBSのPCのファイアウォールでTCP 19000を開放してください。',
        security: 'セキュリティソフトに接続をブロックされる場合は、例外に *.trycloudflare.com を追加してください。'
      },
      status: {
        waiting: '待機中 — 接続ボタンを押してください',
        enterStreamId: '配信IDを入力してください',
        invalidStreamId: '配信IDは半角英数字のみです',
        connecting: '接続中...',
        connectFailed: '接続に失敗しました — URLを確認してください',
        connectTimeout: '接続タイムアウト — 接続先を確認してください',
        receivingWithUrl: '受信中 — {{url}}',
        receiving: '受信中',
        connectionError: '接続エラー — サーバーに接続できません',
        streamIdMismatch: '配信IDが一致しません',
        chOutOfRange: 'CH番号が範囲外です ({{range}})',
        disconnected: '切断されました',
        disconnectedByUser: '切断しました',
        resynced: '再同期しました',
        measuring: '遅延計測中... ({{current}}/{{total}})'
      },
      latency: {
        initialHint: '接続後、OBS側で遅延計測が開始されると、ここに結果が表示されます。',
        measuring: '計測中 ({{current}} / {{total}} ping)',
        estimatedOneWay: '推定片道遅延 (RTT÷2)',
        averageRoundTrip: '平均往復遅延 ({{label}})',
        minRtt: '最小RTT',
        maxRtt: '最大RTT',
        waitingApply: 'OBSが遅延設定を反映するまでお待ちください。',
        applied: 'OBSがサブch遅延を <strong>{{ms}} ms</strong> に自動設定しました'
      },
      quality: {
        good: '良好',
        normal: '普通',
        high: '高遅延'
      },
      format: {
        chRange: '{{min}}〜{{max}}',
        seconds: '{{value}}秒'
      },
      url: {
        streamIdMissing: '(配信ID未入力)'
      },
      codec: {
        opusUnavailable: 'Opusデコーダを利用できません（PCMに切替）',
        opusDecodeFailed: 'Opusデコードに失敗しました（PCMに切替）'
      },
      lang: {
        switchToEn: 'Switch to English',
        switchToJa: '日本語に切り替え'
      }
    }
  },
  en: {
    translation: {
      ui: {
        browserWarning: 'This page may not work correctly in browsers other than Chrome. Chrome is recommended.',
        connectionSettings: 'Connection Settings',
        connectionSettingsNote: '(Usually no changes needed)',
        streamId: 'Stream ID',
        streamIdHint: '(alphanumeric)',
        streamIdPlaceholder: 'myshow2024',
        chNumber: 'Channel',
        resync: 'Resync',
        autoResyncInterval: 'Auto-resync Interval',
        off: 'Off',
        volumeControl: 'Volume Control',
        sampleRate: 'SR',
        channel: 'CH',
        buffer: 'Buffer',
        codec: 'Codec',
        latencyMeasure: 'Latency Measurement'
      },
      button: {
        connect: 'Connect',
        disconnect: 'Disconnect',
        resync: 'Resync',
        resyncCountdown: 'Resync ({{sec}}s)'
      },
      audio: {
        mute: 'Mute',
        muted: 'Muted',
        volume: 'Volume'
      },
      help: {
        chrome: 'Chrome is recommended. If prompted for audio playback permission on first connect, allow it.',
        lan: 'For LAN (direct IP) use, open TCP 19000 in the OBS host PC firewall.',
        security: 'If security software blocks connections, add *.trycloudflare.com to exceptions.'
      },
      status: {
        waiting: 'Idle - press Connect',
        enterStreamId: 'Enter Stream ID',
        invalidStreamId: 'Stream ID must be alphanumeric',
        connecting: 'Connecting...',
        connectFailed: 'Connection failed - check URL',
        connectTimeout: 'Connection timeout - check destination',
        receivingWithUrl: 'Receiving - {{url}}',
        receiving: 'Receiving',
        connectionError: 'Connection error - cannot reach server',
        streamIdMismatch: 'Stream ID mismatch',
        chOutOfRange: 'Channel out of range ({{range}})',
        disconnected: 'Disconnected',
        disconnectedByUser: 'Disconnected',
        resynced: 'Resynced',
        measuring: 'Measuring latency... ({{current}}/{{total}})'
      },
      latency: {
        initialHint: 'After connecting, results will appear here when OBS starts latency measurement.',
        measuring: 'Measuring ({{current}} / {{total}} ping)',
        estimatedOneWay: 'Estimated one-way latency (RTT/2)',
        averageRoundTrip: 'Average round-trip latency ({{label}})',
        minRtt: 'Min RTT',
        maxRtt: 'Max RTT',
        waitingApply: 'Please wait while OBS applies delay settings.',
        applied: 'OBS auto-set sub-channel delay to <strong>{{ms}} ms</strong>'
      },
      quality: {
        good: 'Good',
        normal: 'Normal',
        high: 'High latency'
      },
      format: {
        chRange: '{{min}}-{{max}}',
        seconds: '{{value}} sec'
      },
      url: {
        streamIdMissing: '(Stream ID not set)'
      },
      codec: {
        opusUnavailable: 'Opus decoder unavailable (switched to PCM)',
        opusDecodeFailed: 'Opus decode failed (switched to PCM)'
      },
      lang: {
        switchToEn: 'Switch to English',
        switchToJa: '日本語に切り替え'
      }
    }
  }
};

function detectLanguage() {
  const queryLang = new URLSearchParams(location.search).get('lang');
  if (queryLang && I18N_RESOURCES[queryLang]) {
    localStorage.setItem('receiverLang', queryLang);
    return queryLang;
  }
  const stored = localStorage.getItem('receiverLang');
  if (stored && I18N_RESOURCES[stored]) return stored;
  const nav = (navigator.languages && navigator.languages[0]) || navigator.language || 'ja';
  return nav.toLowerCase().startsWith('ja') ? 'ja' : 'en';
}

const activeLang = detectLanguage();
i18next.init({
  lng: activeLang,
  fallbackLng: 'ja',
  resources: I18N_RESOURCES,
  interpolation: { escapeValue: false },
  returnNull: false,
  returnEmptyString: false,
  initImmediate: false
});
try {
  const buildTimestamp = (document.documentElement.dataset.buildTimestamp || '').trim();
  if (buildTimestamp && !/^@.+@$/.test(buildTimestamp)) {
    console.info('[obs-delay-stream] receiver build:', buildTimestamp);
  }
} catch { }
document.documentElement.lang = activeLang;

function resolveLocalText(lang, key) {
  const root = I18N_RESOURCES[lang] && I18N_RESOURCES[lang].translation;
  if (!root) return undefined;
  const parts = String(key || '').split('.');
  let cur = root;
  for (const p of parts) {
    if (!cur || typeof cur !== 'object' || !(p in cur)) return undefined;
    cur = cur[p];
  }
  return (typeof cur === 'string') ? cur : undefined;
}

function interpolateText(text, opts) {
  const options = opts || {};
  return String(text).replace(/\{\{\s*([a-zA-Z0-9_]+)\s*\}\}/g, (_, k) => {
    const v = options[k];
    return (v === undefined || v === null) ? '' : String(v);
  });
}

function t(key, opts) {
  const options = opts || {};
  const primary = i18next.t(key, options);
  if (typeof primary === 'string' && primary !== '' && primary !== 'undefined') return primary;

  const secondary = i18next.t(key, { ...options, lng: 'ja', defaultValue: key });
  if (typeof secondary === 'string' && secondary !== '' && secondary !== 'undefined') return secondary;

  const local = resolveLocalText(activeLang, key) ?? resolveLocalText('ja', key);
  if (typeof local === 'string') return interpolateText(local, options);

  return String(key);
}

function tr(key, opts, fallbackJa, fallbackEn) {
  const v = t(key, opts);
  if (typeof v === 'string' && v !== '' && v !== 'undefined') return v;
  const fb = (activeLang === 'en') ? fallbackEn : fallbackJa;
  if (typeof fb === 'string' && fb !== '') return interpolateText(fb, opts || {});
  return String(key);
}

function buildLanguageHref(lang) {
  const u = new URL(location.href);
  u.searchParams.set('lang', lang);
  return u.toString();
}

function makeLanguageNode(lang) {
  const label = lang.toUpperCase();
  if (lang === activeLang) {
    const current = document.createElement('span');
    current.className = 'ds-lang-current';
    current.textContent = label;
    current.setAttribute('aria-current', 'true');
    return current;
  }
  const link = document.createElement('a');
  link.className = 'ds-lang-link';
  link.href = buildLanguageHref(lang);
  link.textContent = label;
  link.setAttribute('hreflang', lang);
  link.setAttribute('lang', lang);
  link.setAttribute('aria-label', lang === 'en' ? t('lang.switchToEn') : t('lang.switchToJa'));
  return link;
}

function renderLanguageSwitcher() {
  const root = document.getElementById('langSwitcher');
  if (!root) return;
  root.textContent = '';

  const icon = document.createElement('span');
  icon.className = 'icon is-small';
  icon.innerHTML = '<i class="fas fa-globe" aria-hidden="true"></i>';
  root.appendChild(icon);

  const links = document.createElement('span');
  links.className = 'ds-lang-links';
  links.appendChild(makeLanguageNode('en'));
  const sep = document.createElement('span');
  sep.className = 'ds-lang-sep';
  sep.textContent = '|';
  links.appendChild(sep);
  links.appendChild(makeLanguageNode('ja'));
  root.appendChild(links);
}

function applyStaticI18n() {
  document.querySelectorAll('[data-i18n]').forEach((el) => {
    el.textContent = t(el.dataset.i18n);
  });
  document.querySelectorAll('[data-i18n-placeholder]').forEach((el) => {
    el.setAttribute('placeholder', t(el.dataset.i18nPlaceholder));
  });
  document.querySelectorAll('[data-i18n-title]').forEach((el) => {
    el.setAttribute('title', t(el.dataset.i18nTitle));
  });
  document.querySelectorAll('[data-i18n-aria-label]').forEach((el) => {
    el.setAttribute('aria-label', t(el.dataset.i18nAriaLabel));
  });
  document.querySelectorAll('[data-i18n-seconds]').forEach((el) => {
    el.textContent = t('format.seconds', { value: el.dataset.i18nSeconds });
  });
}
applyStaticI18n();
renderLanguageSwitcher();

// ============================================================
// URL プレビュー
// ============================================================
function getHostDomain() {
  return location.hostname || 'localhost';
}

function buildUrl(ip, sid, ch) {
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

const sidInput = document.getElementById('sidInput');
const chInput = document.getElementById('chInput');
const chHint = document.getElementById('chHint');
function applyChRange(max) {
  MAX_CH = max;
  CH_RANGE_TEXT = t('format.chRange', { min: MIN_CH, max: MAX_CH });
  if (chHint) chHint.textContent = `(${CH_RANGE_TEXT})`;
  if (chInput) {
    chInput.min = String(MIN_CH);
    chInput.max = String(MAX_CH);
    const v = parseInt(chInput.value, 10);
    if (Number.isInteger(v) && v > MAX_CH) chInput.value = String(MAX_CH);
  }
  updateUrlPreview();
}
applyChRange(MAX_CH);

function parseShebangParams() {
  if (!location.hash.startsWith('#!')) return null;
  let raw = location.hash.slice(2);
  if (raw.startsWith('/')) raw = raw.slice(1);
  if (raw.startsWith('?')) raw = raw.slice(1);
  if (!raw) return null;

  const safeDecode = (val) => {
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

function getInitialParams() {
  const hashParams = parseShebangParams();
  if (hashParams && (hashParams.sid || hashParams.ch)) return hashParams;
  return null;
}

function applyUrlParams() {
  const params = getInitialParams();
  if (!params) return;
  const sid = params.sid;
  const ch = params.ch;
  if (sid && sidInput) sidInput.value = sid;
  if (ch && chInput) {
    const chNum = parseInt(ch, 10);
    if (!Number.isNaN(chNum) && chNum >= MIN_CH && chNum <= MAX_CH) {
      chInput.value = String(chNum);
    }
  }
  if (sid && ch) fetchMemoPreview(sid, ch);
}
applyUrlParams();

function isChromeBrowser() {
  const uaData = navigator.userAgentData;
  if (uaData && Array.isArray(uaData.brands)) {
    return uaData.brands.some((brand) => brand.brand === 'Google Chrome');
  }
  const ua = navigator.userAgent || '';
  const hasChromeToken = /Chrome\/\d+/.test(ua) || /CriOS\/\d+/.test(ua);
  if (!hasChromeToken) return false;
  const nonChromeToken = /(Edg|OPR|Opera|SamsungBrowser|UCBrowser|YaBrowser|Vivaldi)\//.test(ua);
  return !nonChromeToken;
}

function updateBrowserWarning() {
  const warning = document.getElementById('browserWarning');
  if (!warning) return;
  warning.hidden = isChromeBrowser();
}
updateBrowserWarning();

function loadConfig() {
  fetch('/config', { cache: 'no-store' })
    .then(r => r.ok ? r.json() : null)
    .then(cfg => {
      if (!cfg) return;
      const v = Number.isInteger(cfg.active_ch) ? cfg.active_ch : null;
      if (Number.isInteger(v) && v >= MIN_CH) applyChRange(v);
    })
    .catch(() => {});
}
loadConfig();

function fetchMemoPreview(sid, ch) {
  const chNum = parseInt(ch, 10);
  if (!sid || !Number.isInteger(chNum) || chNum < MIN_CH || chNum > MAX_CH) return;
  const memoEl = document.getElementById('infoMemo');
  if (!memoEl) return;
  const url = `/memo?sid=${encodeURIComponent(sid)}&ch=${encodeURIComponent(String(chNum))}`;
  fetch(url, { cache: 'no-store' })
    .then(r => r.ok ? r.json() : null)
    .then(data => {
      if (!data || typeof data.memo !== 'string') return;
      updateMemoDisplay(memoEl, data.memo);
    })
    .catch(() => {});
}

function updateUrlPreview() {
  const sid = sidInput.value.trim();
  const ch = chInput.value || '1';
  const preview = document.getElementById('urlPreview');
  if (preview) preview.textContent = buildUrl(hostDomain, sid, ch);
}

function updateShebang(sid, ch) {
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
function onInputChange() {
  updateUrlPreview();
  const sid = sidInput.value.trim();
  const ch = chInput.value || '1';
  updateShebang(sid, ch);
}
['sidInput', 'chInput'].forEach(id =>
  document.getElementById(id).addEventListener('input', onInputChange));
updateUrlPreview();

// ============================================================
// AudioContext
// ============================================================
let ws = null;
let actx = null;
let gainNode = null;
let nextTime = 0;
const AHEAD = 0.12;
const MAGIC_AUDI = 0x41554449;
const MAGIC_OPUS = 0x4f505553;
let muted = false;
let connecting = false;
let lastWsError = false;
let connectTimer = null;
const CONNECT_TIMEOUT_MS = 7000;
let syncAvailable = false;

// 制御メッセージ / 再同期の状態
let pingCount = 0;
let autoSyncTimer = null;
let syncCountdown = 0;
let syncTickTimer = null;
let currentSyncInterval = 0;  // 0=オフ
let pendingSyncInterval = 0;

// WebCodecs (Opus)
let opusDecoder = null;
let opusConfig = null;
let opusInitPromise = null;
let opusQueue = [];
let opusTsUs = 0;
let opusErrored = false;
let opusErrorReported = false;
let opusMode = 'auto'; // 'auto' | 'webcodecs' | 'wasm'
let opusWebCodecsState = 'idle'; // idle | pending | ready | failed
let opusWasmState = 'idle'; // idle | pending | ready | failed
let opusWasmDecoder = null;
let opusWasmConfig = null;
let opusWasmInitPromise = null;
let opusWasmLibPromise = null;
let opusWasmErrorStreak = 0;
let opusWasmLastErrorTs = 0;
let pcmFallbackRequested = false;
let pcmFallbackReason = '';

function reportOpusError(msg) {
  if (opusErrorReported) return;
  opusErrorReported = true;
  setStatus(msg, 'err');
  try { console.warn('[Opus]', msg); } catch { }
}

function sendPcmFallbackIfPossible() {
  if (!pcmFallbackRequested) return;
  if (ws && ws.readyState === WebSocket.OPEN) {
    const payload = { type: 'audio_codec', mode: 'pcm' };
    if (pcmFallbackReason) payload.reason = pcmFallbackReason;
    try { ws.send(JSON.stringify(payload)); } catch { }
  }
}

function requestPcmFallback(reason) {
  if (pcmFallbackRequested) return;
  pcmFallbackRequested = true;
  pcmFallbackReason = reason || '';
  sendPcmFallbackIfPossible();
}

function disableOpus(reason, msg) {
  if (opusErrored) return;
  opusErrored = true;
  requestPcmFallback(reason);
  reportOpusError(msg || t('codec.opusDecodeFailed'));
  opusQueue = [];
}

function markWebCodecsFailed(reason) {
  if (opusWebCodecsState === 'failed') return;
  opusWebCodecsState = 'failed';
  if (reason) {
    try { console.warn('[Opus][WebCodecs]', reason); } catch { }
  }
}

function markWasmFailed(reason) {
  if (opusWasmState === 'failed') return;
  opusWasmState = 'failed';
  if (reason) {
    try { console.warn('[Opus][WASM]', reason); } catch { }
  }
}

function noteWasmError() {
  const now = Date.now();
  if (now - opusWasmLastErrorTs > 2000) opusWasmErrorStreak = 0;
  opusWasmLastErrorTs = now;
  opusWasmErrorStreak++;
  return opusWasmErrorStreak;
}

function clearWasmErrors() {
  opusWasmErrorStreak = 0;
  opusWasmLastErrorTs = 0;
}

function loadOpusWasmLibrary() {
  if (opusWasmLibPromise) return opusWasmLibPromise;
  opusWasmLibPromise = new Promise((resolve, reject) => {
    const s = document.createElement('script');
    s.async = true;
    s.src = 'third_party/opus-decoder/opus-decoder.min.js';
    s.onload = () => resolve(true);
    s.onerror = () => reject(new Error('opus-decoder load failed'));
    document.head.appendChild(s);
  });
  return opusWasmLibPromise;
}

// VUメーター
const NUM_BARS = 20;
const meterEl = document.getElementById('meter');
const bars = [];
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
function sliderToGain(val) {
  // 0→-∞dB, 50→-20dB, 84→-3.0dB, 100→0dB (二乗カーブ)
  if (val <= 0) return 0;
  return Math.pow(val / 100, 2);
}
function gainToDb(gain) {
  if (gain <= 0) return -Infinity;
  return 20 * Math.log10(gain);
}
function formatDb(gain) {
  if (gain <= 0) return '−∞ dB';
  const db = gainToDb(gain);
  return (db >= 0 ? '+' : '') + db.toFixed(1) + ' dB';
}

function onVolumeChange(val) {
  const v = parseInt(val, 10);
  localStorage.setItem('volume', v);
  const gain = sliderToGain(v);
  if (gainNode && !muted) gainNode.gain.value = gain;
  const slider = document.getElementById('volSlider');
  slider.disabled = muted;
  slider.classList.toggle('is-primary', !muted);
  const dbEl = document.getElementById('dbDisplay');
  dbEl.textContent = muted ? t('audio.muted') : formatDb(gain);
  const dbClasses = ['tag', 'is-medium', 'has-background-dark', 'has-text-white', 'is-family-monospace'];
  if (muted) {
    dbClasses.push('has-text-grey');
  }
  dbEl.className = dbClasses.join(' ');
}

function toggleMute() {
  muted = !muted;
  const btn = document.getElementById('muteBtn');
  btn.className = muted ? 'button is-ghost has-text-grey' : 'button is-primary';
  const slider = document.getElementById('volSlider');
  slider.disabled = muted;
  slider.classList.toggle('is-primary', !muted);
  const iconEl = document.getElementById('muteIcon');
  if (iconEl) iconEl.className = muted ? 'fas fa-volume-mute' : 'fas fa-volume-up';
  if (gainNode) {
    gainNode.gain.value = muted ? 0
      : sliderToGain(parseInt(document.getElementById('volSlider').value, 10));
  }
  const val = parseInt(document.getElementById('volSlider').value, 10);
  const gain = sliderToGain(val);
  const dbEl = document.getElementById('dbDisplay');
  dbEl.textContent = muted ? t('audio.muted') : formatDb(gain);
  const dbClasses = ['tag', 'is-medium', 'has-background-dark', 'has-text-white', 'is-family-monospace'];
  if (muted) {
    dbClasses.push('has-text-grey');
  }
  dbEl.className = dbClasses.join(' ');
}

// 初期表示（localStorageから復元）
{
  const savedVol = localStorage.getItem('volume');
  const initVol = savedVol !== null ? Number(savedVol) : 84;
  document.getElementById('volSlider').value = initVol;
  onVolumeChange(initVol);

  const savedSync = localStorage.getItem('syncInterval');
  if (savedSync !== null) {
    document.getElementById('syncIntervalSelect').value = savedSync;
    onSyncIntervalChange(savedSync);
  }
}

// ============================================================
// 接続
// ============================================================
function setStatus(msg, cls = '') {
  const el = document.getElementById('statusBar');
  const textEl = document.getElementById('statusText');
  const iconEl = document.getElementById('statusIcon');
  if (textEl) textEl.textContent = msg;
  const classes = ['notification'];
  if (cls === 'ok') classes.push('is-success');
  else if (cls === 'err') classes.push('is-danger');
  else if (cls === 'mea') classes.push('is-warning');
  else classes.push('is-dark');
  el.className = classes.join(' ');
  let icon = 'fa-info-circle';
  if (cls === 'ok') icon = 'fa-check-circle';
  else if (cls === 'err') icon = 'fa-times-circle';
  else if (cls === 'mea') icon = 'fa-exclamation-triangle';
  if (iconEl) iconEl.className = `fas ${icon}`;
}

function setCodecLabel(text) {
  const el = document.getElementById('infoCodec');
  if (el) el.textContent = text || '—';
}

function clearConnectTimer() {
  if (connectTimer) { clearTimeout(connectTimer); connectTimer = null; }
}

function setInputsEnabled(enabled) {
  document.getElementById('sidInput').disabled = !enabled;
  document.getElementById('chInput').disabled = !enabled;
}

function setDisconnectedUi() {
  document.getElementById('connectBtn').disabled = false;
  document.getElementById('stopBtn').disabled = true;
  document.getElementById('syncBtn').disabled = true;
  enableSyncOptions(false);
  document.getElementById('latencyCard').classList.remove('has-background-info-light');
  setInputsEnabled(true);
  setCodecLabel('—');
}

function connect() {
  if (connecting) return;
  if (ws) ws.close();
  if (!actx) {
    actx = new (window.AudioContext || window.webkitAudioContext)();
    gainNode = actx.createGain();
    gainNode.gain.value = sliderToGain(
      parseInt(document.getElementById('volSlider').value, 10));
    gainNode.connect(actx.destination);
  }
  nextTime = actx.currentTime + AHEAD;

  const sid = document.getElementById('sidInput').value.trim();
  const ch = parseInt(document.getElementById('chInput').value, 10);

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
  document.getElementById('connectBtn').disabled = true;
  try {
    ws = new WebSocket(url);
  } catch (e) {
    connecting = false;
    setStatus(t('status.connectFailed'), 'err');
    document.getElementById('connectBtn').disabled = false;
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
    document.getElementById('connectBtn').disabled = true;
    document.getElementById('stopBtn').disabled = false;
    document.getElementById('syncBtn').disabled = false;
    enableSyncOptions(true);
    document.getElementById('latencyCard').classList.add('has-background-info-light');
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
  wsLocal.onmessage = ev => {
    if (ws !== wsLocal) return;
    if (ev.data instanceof ArrayBuffer) handleAudio(ev.data);
    else handleControl(ev.data);
  };
}

function disconnect() {
  stopAutoSync();
  clearConnectTimer();
  connecting = false;
  document.getElementById('syncBtn').disabled = true;
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
function handleAudio(buf) {
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

function playBuffer(abuf) {
  if (!abuf) return;
  const src = actx.createBufferSource();
  src.buffer = abuf;
  src.connect(gainNode);

  const now = actx.currentTime;
  if (nextTime < now + 0.01) nextTime = now + AHEAD;
  src.start(nextTime);
  nextTime += abuf.duration;

  const bufMs = Math.round((nextTime - actx.currentTime) * 1000);
  document.getElementById('infoBuf').textContent = bufMs + ' ms';
  updateMeter(abuf.getChannelData(0));
}

function handlePcm16(buf, sampleRate, channels, frameCount) {
  if (!actx) return;
  document.getElementById('infoSR').textContent = sampleRate + ' Hz';
  document.getElementById('infoCH').textContent = channels + 'ch';
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

function handleOpus(buf, sampleRate, channels, frameCount) {
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

function ensureWebCodecsDecoder(sampleRate, channels) {
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
  if (typeof AudioDecoder === 'undefined') {
    markWebCodecsFailed('no_audio_decoder');
    return false;
  }

  const config = { codec: 'opus', sampleRate, numberOfChannels: channels };
  opusWebCodecsState = 'pending';
  opusInitPromise = AudioDecoder.isConfigSupported(config).then(s => {
    if (!s.supported) {
      markWebCodecsFailed('config_unsupported');
      return false;
    }
    opusDecoder = new AudioDecoder({
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

function ensureWasmDecoder(sampleRate, channels) {
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
    const mod = window['opus-decoder'];
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

function flushOpusQueue() {
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

function decodeOpusPacketWebCodecs(buf, sampleRate, channels, frameCount) {
  if (!opusDecoder) return;
  document.getElementById('infoSR').textContent = sampleRate + ' Hz';
  document.getElementById('infoCH').textContent = channels + 'ch';

  const payload = new Uint8Array(buf, 16);
  const durationUs = frameCount > 0 ? Math.round(frameCount * 1000000 / sampleRate) : 0;
  const chunk = new EncodedAudioChunk({
    type: 'key',
    timestamp: opusTsUs,
    duration: durationUs,
    data: payload
  });
  opusDecoder.decode(chunk);
  if (durationUs > 0) opusTsUs += durationUs;
}

function onOpusDecoded(ad) {
  const abuf = audioDataToBuffer(ad);
  if (abuf) {
    setCodecLabel('Opus (WebCodecs)');
    playBuffer(abuf);
  }
  ad.close();
}

function audioDataToBuffer(ad) {
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

function decodeOpusPacketWasm(buf, sampleRate, channels, frameCount) {
  if (!opusWasmDecoder || !actx) return;
  document.getElementById('infoSR').textContent = sampleRate + ' Hz';
  document.getElementById('infoCH').textContent = channels + 'ch';

  const payload = new Uint8Array(buf, 16);
  let decoded;
  try {
    decoded = opusWasmDecoder.decodeFrame(payload);
  } catch (e) {
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
function handleControl(text) {
  let msg; try { msg = JSON.parse(text); } catch { return; }

  switch (msg.type) {
    case 'session_info':
      // サーバーからセッション確認情報を受信
      if (msg.stream_id) document.getElementById('sidInput').value = msg.stream_id;
      if (msg.ch !== undefined && msg.ch !== null) {
        document.getElementById('chInput').value = String(msg.ch);
      }
      {
        const memoEl = document.getElementById('infoMemo');
        if (memoEl) updateMemoDisplay(memoEl, msg.memo);
      }
      break;

    case 'memo':
      {
        const memoEl = document.getElementById('infoMemo');
        if (memoEl) updateMemoDisplay(memoEl, msg.memo);
      }
      break;

    case 'ping':
      if (ws && ws.readyState === WebSocket.OPEN)
        ws.send(JSON.stringify({ type: 'pong', seq: msg.seq }));
      showMeasuring(++pingCount);
      break;

    case 'latency_result':
      showLatencyResult(msg);
      pingCount = 0;
      break;

    case 'apply_delay':
      {
        const reason = (typeof msg.reason === 'string') ? msg.reason : '';
        if (reason === 'manual_adjust') break;
        if (reason === 'auto_measure') {
          showApplied(msg.ms);
          break;
        }
        // 旧サーバ互換: reason未送信時は「計測→適用待ち」の文脈だけ表示
        if (document.getElementById('waitingApply')) {
          showApplied(msg.ms);
        }
      }
      break;
  }
}

function updateMemoDisplay(memoEl, memoText) {
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
function resync() {
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

function startAutoSync(interval) {
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

function stopAutoSync() {
  if (syncTickTimer) { clearInterval(syncTickTimer); syncTickTimer = null; }
  currentSyncInterval = 0;
  updateCountdown();
}

function onSyncIntervalChange(interval) {
  const value = Number(interval);
  localStorage.setItem('syncInterval', value);
  pendingSyncInterval = Number.isFinite(value) ? value : 0;
  if (pendingSyncInterval === 0) {
    stopAutoSync();
    return;
  }
  if (syncAvailable) {
    startAutoSync(pendingSyncInterval);
  }
}

function enableSyncOptions(enabled) {
  syncAvailable = enabled;
  if (!enabled) {
    stopAutoSync();
    return;
  }
  onSyncIntervalChange(document.getElementById('syncIntervalSelect').value);
}

function updateCountdown() {
  const labelEl = document.getElementById('syncBtnLabel');
  if (syncTickTimer && currentSyncInterval > 0) {
    labelEl.textContent = tr(
      'button.resyncCountdown',
      { sec: syncCountdown },
      '再同期（あと{{sec}}秒）',
      'Resync ({{sec}}s)'
    );
  } else {
    labelEl.textContent = tr('button.resync', {}, '再同期', 'Resync');
  }
}

// ============================================================
// 計測UI
// ============================================================
function showMeasuring(n) {
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
  document.getElementById('latencyContent').innerHTML = `
<div class="measuring-box">
  ${measuringText}<span class="dots"><span>.</span><span>.</span><span>.</span></span>
  <progress class="progress is-small is-warning" style="margin-top:6px" value="${n}" max="${PING_SAMPLES}">${n}</progress>
</div>`;
}

function showLatencyResult(r) {
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
  document.getElementById('latencyContent').innerHTML = `
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
  document.getElementById('latencyCard').classList.add('has-background-info-light');
}

function showApplied(ms) {
  document.querySelectorAll('#waitingApply').forEach((el) => el.remove());
  document.querySelectorAll('#appliedDelayNote').forEach((el) => el.remove());
  const appliedText = tr(
    'latency.applied',
    { ms: parseFloat(ms).toFixed(1) },
    'OBSがサブch遅延を <strong>{{ms}} ms</strong> に自動設定しました',
    'OBS auto-set sub-channel delay to <strong>{{ms}} ms</strong>'
  );
  document.getElementById('latencyContent').innerHTML +=
    `<div class="notification is-success py-3 px-4 mt-3" id="appliedDelayNote">
  <i class="fas fa-check-circle mr-1"></i>
  ${appliedText}
</div>`;
}

// ============================================================
// VUメーター
// ============================================================
function updateMeter(samples) {
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
