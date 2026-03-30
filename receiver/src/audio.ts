import { state } from './state';
import { NUM_BARS } from './constants';
import { scheduleSustain } from './sustain';
import {
  volSlider,
  dbDisplay,
  muteBtn,
  muteIcon,
  meterEl,
  infoBuf,
  infoSR,
  infoCH,
} from './elements';
import { setCodecLabel } from './ui';
import { t } from './i18n';
import type { WindowWithWebkitAudioContext } from './types';

const ANALYZER_FFT_SIZE = 512;
const ANALYZER_SMOOTHING = 0.82;
const METER_ANALYZER_SMOOTHING = 0;
const ANALYZER_MIN_DB = -90;
const ANALYZER_MAX_DB = -24;
const ANALYZER_MIN_HZ = 60;
const ANALYZER_MAX_HZ = 12000;
const METER_UPDATE_INTERVAL_MS = 33;
const METER_DIGITAL_STEPS = 24;

// ============================================================
// 音量
// ============================================================

export function sliderToGain(val: number): number {
  if (val <= 0) return 0;
  return Math.pow(val / 100, 2);
}

export function gainToDb(gain: number): number {
  if (gain <= 0) return -Infinity;
  return 20 * Math.log10(gain);
}

export function formatDb(gain: number): string {
  if (gain <= 0) return '−∞ dB';
  const db = gainToDb(gain);
  return (db >= 0 ? '+' : '') + db.toFixed(1) + ' dB';
}

function updateVolumeDisplay(gain: number): void {
  volSlider.disabled = state.muted;
  volSlider.classList.toggle('is-primary', !state.muted);
  dbDisplay.textContent = state.muted ? t('audio.muted') : formatDb(gain);
  const dbClasses = [
    'tag',
    'is-medium',
    'has-background-dark',
    'has-text-white',
    'is-family-monospace',
  ];
  if (state.muted) dbClasses.push('has-text-grey');
  dbDisplay.className = dbClasses.join(' ');
}

export function onVolumeChange(val: number | string): void {
  const v = typeof val === 'number' ? val : parseInt(val, 10);
  const safeV = Number.isFinite(v) ? v : 0;
  localStorage.setItem('volume', String(safeV));
  const gain = sliderToGain(safeV);
  if (state.gainNode && !state.muted) state.gainNode.gain.value = gain;
  updateVolumeDisplay(gain);
}

export function toggleMute(): void {
  state.muted = !state.muted;
  muteBtn.className = state.muted
    ? 'button is-ghost has-text-grey'
    : 'button is-primary';
  if (muteIcon)
    muteIcon.className = state.muted
      ? 'fas fa-volume-mute'
      : 'fas fa-volume-up';
  if (state.gainNode) {
    state.gainNode.gain.value = state.muted
      ? 0
      : sliderToGain(parseInt(volSlider.value, 10));
  }
  meterEl.classList.toggle('is-muted', state.muted);
  lastMutedState = state.muted;
  const gain = sliderToGain(parseInt(volSlider.value, 10));
  updateVolumeDisplay(gain);
}

// ============================================================
// AudioContext
// ============================================================

export function ensureAudioContext(sampleRate?: number): boolean {
  if (state.actx) {
    if (!sampleRate || state.actx.sampleRate === sampleRate) return true;
    // サンプルレート不一致 — 再生成して per-buffer リサンプリングノイズを回避
    try { state.actx.close(); } catch { /* */ }
    state.actx = null;
    state.gainNode = null;
    state.xfade = [null, null];
    state.nextTime = 0;
    state.lastBuffer = null;
  }
  const AudioContextCtor =
    window.AudioContext ||
    (window as WindowWithWebkitAudioContext).webkitAudioContext;
  if (!AudioContextCtor) return false;
  try {
    state.actx = new AudioContextCtor(sampleRate ? { sampleRate } : undefined);
  } catch {
    state.actx = new AudioContextCtor();
  }
  state.gainNode = state.actx.createGain();
  state.gainNode.gain.value = sliderToGain(parseInt(volSlider.value, 10));
  meterAnalyserNode = state.actx.createAnalyser();
  meterAnalyserNode.fftSize = ANALYZER_FFT_SIZE;
  meterAnalyserNode.smoothingTimeConstant = METER_ANALYZER_SMOOTHING;
  meterAnalyserNode.minDecibels = ANALYZER_MIN_DB;
  meterAnalyserNode.maxDecibels = ANALYZER_MAX_DB;
  analyserNode = state.actx.createAnalyser();
  analyserNode.fftSize = ANALYZER_FFT_SIZE;
  analyserNode.smoothingTimeConstant = ANALYZER_SMOOTHING;
  analyserNode.minDecibels = ANALYZER_MIN_DB;
  analyserNode.maxDecibels = ANALYZER_MAX_DB;
  state.gainNode.connect(meterAnalyserNode);
  meterAnalyserNode.connect(analyserNode);
  analyserNode.connect(state.actx.destination);
  spectrumData = new Uint8Array(meterAnalyserNode.frequencyBinCount);
  spectrumRanges = buildSpectrumRanges(state.actx.sampleRate, meterAnalyserNode.fftSize);
  // クロスフェード用ノード 2 本を gainNode の手前に配置
  state.xfade[0] = state.actx.createGain();
  state.xfade[1] = state.actx.createGain();
  state.xfade[0].connect(state.gainNode);
  state.xfade[1].connect(state.gainNode);
  state.xfade[1].gain.value = 0;   // 非アクティブ側は無音
  state.xfadeIdx = 0;
  return true;
}

// ============================================================
// バッファ再生
// ============================================================

export function playBuffer(abuf: AudioBuffer | null): void {
  if (!abuf || !state.actx || !state.gainNode) return;
  const dest = state.xfade[state.xfadeIdx] || state.gainNode;

  const now = state.actx.currentTime;
  // アンダーラン検出: nextTime が過去に落ちた場合、サステインで隙間を埋める
  if (state.nextTime < now + 0.02) {
    const gapStart = Math.max(state.nextTime, now);
    const newNext = now + state.playbackBuffer;
    const gapSec = newNext - gapStart;
    if (gapSec > 0.005) {
      scheduleSustain(dest, gapStart, gapSec);
    }
    state.nextTime = newNext;
  }

  state.lastBuffer = abuf;
  const src = state.actx.createBufferSource();
  src.buffer = abuf;
  src.connect(dest);
  src.start(state.nextTime);
  const bufMs = Math.round((state.nextTime - state.actx.currentTime) * 1000);
  state.nextTime += abuf.duration;

  infoBuf.textContent = bufMs + ' ms';
  updateMeter();
}

// ============================================================
// PCM デコード
// ============================================================

export function updateAudioInfo(
  sampleRate: number,
  channels: number,
): void {
  infoSR.textContent = sampleRate + ' Hz';
  infoCH.textContent = channels === 1 ? 'Mono' : channels === 2 ? 'Stereo' : channels + 'ch';
}

export function handlePcm16(
  buf: ArrayBuffer,
  sampleRate: number,
  channels: number,
  frameCount: number,
): void {
  ensureAudioContext(sampleRate);
  if (!state.actx) return;
  updateAudioInfo(sampleRate, channels);
  setCodecLabel('PCM');

  const pcm = new Int16Array(buf, 16);
  const abuf = state.actx.createBuffer(channels, frameCount, sampleRate);
  for (let c = 0; c < channels; c++) {
    const d = abuf.getChannelData(c);
    for (let f = 0; f < frameCount; f++) {
      d[f] = pcm[f * channels + c] / 32768;
    }
  }
  playBuffer(abuf);
}

// ============================================================
// スペクトラムメーター
// ============================================================

let bars: HTMLDivElement[] = [];
let lastLevelSteps: number[] = [];
let analyserNode: AnalyserNode | null = null;
let meterAnalyserNode: AnalyserNode | null = null;
let spectrumData = new Uint8Array(0);
let spectrumRanges: Array<[number, number]> = [];
let lastMeterUpdateAt = 0;
let lastMutedState = false;

function buildSpectrumRanges(
  sampleRate: number,
  fftSize: number,
): Array<[number, number]> {
  const binCount = Math.max(1, Math.floor(fftSize / 2));
  const nyquist = sampleRate / 2;
  const minHz = Math.max(20, Math.min(ANALYZER_MIN_HZ, nyquist));
  const maxHz = Math.max(minHz, Math.min(ANALYZER_MAX_HZ, nyquist));
  const hzPerBin = nyquist / binCount || 1;
  const ranges: Array<[number, number]> = [];
  let prevEnd = -1;

  for (let i = 0; i < NUM_BARS; i++) {
    const startHz =
      minHz * Math.pow(maxHz / minHz, i / NUM_BARS);
    const endHz =
      minHz * Math.pow(maxHz / minHz, (i + 1) / NUM_BARS);
    let start = Math.floor(startHz / hzPerBin);
    let end = Math.floor(endHz / hzPerBin);

    if (start <= prevEnd) start = prevEnd + 1;
    if (start >= binCount) start = binCount - 1;
    if (end < start) end = start;
    if (end >= binCount) end = binCount - 1;

    ranges.push([start, end]);
    prevEnd = end;
  }
  return ranges;
}

export function initMeter(): void {
  bars = [];
  lastLevelSteps = new Array(NUM_BARS).fill(-1);
  lastMutedState = state.muted;
  meterEl.classList.toggle('is-muted', state.muted);
  meterEl.innerHTML = '';
  for (let i = 0; i < NUM_BARS; i++) {
    const b = document.createElement('div');
    b.className = 'bar';
    b.style.setProperty('--level', '0');
    meterEl.appendChild(b);
    bars.push(b);
  }
}

function updateMeter(): void {
  if (!meterAnalyserNode || spectrumData.length === 0 || bars.length === 0) return;
  const now = performance.now();
  if (now - lastMeterUpdateAt < METER_UPDATE_INTERVAL_MS) return;
  lastMeterUpdateAt = now;
  if (lastMutedState !== state.muted) {
    meterEl.classList.toggle('is-muted', state.muted);
    lastMutedState = state.muted;
  }

  meterAnalyserNode.getByteFrequencyData(spectrumData);
  for (let i = 0; i < bars.length; i++) {
    const range = spectrumRanges[i] || [0, 0];
    const start = range[0];
    const end = range[1];
    let sum = 0;
    for (let j = start; j <= end; j++) {
      sum += spectrumData[j] || 0;
    }
    const avg = sum / (end - start + 1);
    const normalized = avg / 255;
    const level = Math.min(1, Math.max(0, normalized));
    const levelStep = Math.round(level * METER_DIGITAL_STEPS);
    if (lastLevelSteps[i] !== levelStep) {
      const quantized = levelStep / METER_DIGITAL_STEPS;
      bars[i].style.setProperty('--level', quantized.toFixed(3));
      lastLevelSteps[i] = levelStep;
    }
  }
}
