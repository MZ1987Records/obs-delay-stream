import '@fortawesome/fontawesome-free/css/all.min.css';
import 'bulma/css/bulma.min.css';
import '../css/ui.css';
import { applyStaticI18n, initI18n, renderLanguageSwitcher } from './i18n';
import { DEFAULT_MAX_CH, DEFAULT_VOLUME } from './constants';
import { sidInput, chInput, volSlider, syncIntervalSelect } from './elements';
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
} from './ui';
import { connect, disconnect } from './connection';

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
