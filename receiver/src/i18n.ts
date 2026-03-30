import i18next from 'i18next';

type I18nResource = { translation: Record<string, unknown> };
export type I18nOptions = Record<string, string | number | boolean | null | undefined>;

const I18N_RESOURCES: Record<'ja' | 'en', I18nResource> = {
  ja: {
    translation: {
      ui: {
        browserWarning: 'このページはChrome以外のブラウザでは正常に動作しない場合があります。Chromeの利用を推奨します。',
        connectionSettings: '接続設定',
        connectionSettingsNote: '（通常は変更不要）',
        connectionInfo: '接続情報',
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
        codeNotFound: 'チャンネル識別コードが見つかりません — URLを確認してください',
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
        appliedPrefix: 'OBSがチャンネル遅延を ',
        appliedSuffix: ' に自動設定しました'
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
        connectionInfo: 'Connection Info',
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
        codeNotFound: 'Channel code not found - check URL',
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
        appliedPrefix: 'OBS auto-set channel delay to ',
        appliedSuffix: ''
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

export type Lang = keyof typeof I18N_RESOURCES;

function isLang(value: string): value is Lang {
  return Object.prototype.hasOwnProperty.call(I18N_RESOURCES, value);
}

function detectLanguage(): Lang {
  const queryLang = new URLSearchParams(location.search).get('lang');
  if (queryLang && isLang(queryLang)) {
    localStorage.setItem('receiverLang', queryLang);
    return queryLang;
  }
  const stored = localStorage.getItem('receiverLang');
  if (stored && isLang(stored)) return stored;
  const nav = (navigator.languages && navigator.languages[0]) || navigator.language || 'ja';
  return nav.toLowerCase().startsWith('ja') ? 'ja' : 'en';
}

export const activeLang: Lang = detectLanguage();

export function initI18n(): void {
  i18next.init({
    lng: activeLang,
    fallbackLng: 'ja',
    resources: I18N_RESOURCES,
    interpolation: { escapeValue: false },
    returnNull: false,
    returnEmptyString: false,
    initImmediate: false
  });
  document.documentElement.lang = activeLang;
}

function resolveLocalText(lang: Lang, key: string): string | undefined {
  const root = I18N_RESOURCES[lang] && I18N_RESOURCES[lang].translation;
  if (!root) return undefined;
  const parts = String(key || '').split('.');
  let cur: unknown = root;
  for (const p of parts) {
    if (!cur || typeof cur !== 'object' || !(p in (cur as Record<string, unknown>))) return undefined;
    cur = (cur as Record<string, unknown>)[p];
  }
  return (typeof cur === 'string') ? cur : undefined;
}

function interpolateText(text: string, opts?: I18nOptions): string {
  const options = opts || {};
  return String(text).replace(/\{\{\s*([a-zA-Z0-9_]+)\s*\}\}/g, (_, k: string) => {
    const v = options[k];
    return (v === undefined || v === null) ? '' : String(v);
  });
}

export function t(key: string, opts?: I18nOptions): string {
  const options = opts || {};
  const primary = i18next.t(key, options);
  if (typeof primary === 'string' && primary !== '' && primary !== 'undefined') return primary;

  const secondary = i18next.t(key, { ...options, lng: 'ja', defaultValue: key });
  if (typeof secondary === 'string' && secondary !== '' && secondary !== 'undefined') return secondary;

  const local = resolveLocalText(activeLang, key) ?? resolveLocalText('ja', key);
  if (typeof local === 'string') return interpolateText(local, options);

  return String(key);
}

export function tr(
  key: string,
  opts: I18nOptions | undefined,
  fallbackJa: string,
  fallbackEn: string
): string {
  const v = t(key, opts);
  if (typeof v === 'string' && v !== '' && v !== 'undefined') return v;
  const fb = (activeLang === 'en') ? fallbackEn : fallbackJa;
  if (typeof fb === 'string' && fb !== '') return interpolateText(fb, opts || {});
  return String(key);
}

function buildLanguageHref(lang: Lang): string {
  const u = new URL(location.href);
  u.searchParams.set('lang', lang);
  return u.toString();
}

function makeLanguageNode(lang: Lang): HTMLElement {
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

export function renderLanguageSwitcher(): void {
  const root = document.getElementById('langSwitcher');
  if (!root) return;
  root.textContent = '';

  const icon = document.createElement('span');
  icon.className = 'icon is-small';
  const globe = document.createElement('i');
  globe.className = 'fas fa-globe';
  globe.setAttribute('aria-hidden', 'true');
  icon.appendChild(globe);
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

export function applyStaticI18n(): void {
  document.querySelectorAll<HTMLElement>('[data-i18n]').forEach((el) => {
    el.textContent = t(el.dataset.i18n || '');
  });
  document.querySelectorAll<HTMLElement>('[data-i18n-placeholder]').forEach((el) => {
    el.setAttribute('placeholder', t(el.dataset.i18nPlaceholder || ''));
  });
  document.querySelectorAll<HTMLElement>('[data-i18n-title]').forEach((el) => {
    el.setAttribute('title', t(el.dataset.i18nTitle || ''));
  });
  document.querySelectorAll<HTMLElement>('[data-i18n-aria-label]').forEach((el) => {
    el.setAttribute('aria-label', t(el.dataset.i18nAriaLabel || ''));
  });
  document.querySelectorAll<HTMLElement>('[data-i18n-seconds]').forEach((el) => {
    el.textContent = t('format.seconds', { value: el.dataset.i18nSeconds });
  });
}
