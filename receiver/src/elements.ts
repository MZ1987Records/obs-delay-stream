import {
  getOptionalElement,
  getRequiredButtonElement,
  getRequiredElement,
  getRequiredInputElement,
  getRequiredSelectElement,
} from './dom';

// 接続設定
export const sidInput = getRequiredInputElement('sidInput');
export const codeInput = getRequiredInputElement('codeInput');
export const urlPreview = getOptionalElement<HTMLElement>('urlPreview');
export const browserWarningBlock = getOptionalElement<HTMLElement>('browserWarningBlock');

// ステータス
export const statusBar = getRequiredElement<HTMLElement>('statusBar');
export const statusText = getOptionalElement<HTMLElement>('statusText');
export const statusIcon = getOptionalElement<HTMLElement>('statusIcon');

// 接続ボタン
export const connectBtn = getRequiredButtonElement('connectBtn');
export const stopBtn = getRequiredButtonElement('stopBtn');
export const syncBtn = getRequiredButtonElement('syncBtn');

// 音声情報
export const infoCodec = getRequiredElement<HTMLElement>('infoCodec');
export const infoBuf = getRequiredElement<HTMLElement>('infoBuf');
export const infoSR = getRequiredElement<HTMLElement>('infoSR');
export const infoCH = getRequiredElement<HTMLElement>('infoCH');

// レイテンシ計測
export const latencyCard = getRequiredElement<HTMLElement>('latencyCard');
export const latencyContent = getRequiredElement<HTMLElement>('latencyContent');

// 音量
export const volSlider = getRequiredInputElement('volSlider');
export const dbDisplay = getRequiredElement<HTMLElement>('dbDisplay');
export const muteBtn = getRequiredButtonElement('muteBtn');
export const muteIcon = getOptionalElement<HTMLElement>('muteIcon');

// 同期
export const syncIntervalSelect = getRequiredSelectElement('syncIntervalSelect');
export const syncBtnLabel = getRequiredElement<HTMLElement>('syncBtnLabel');

// VUメーター
export const meterEl = getRequiredElement<HTMLDivElement>('meter');
