import { state } from './state';
import { NUM_BARS, RESYNC_RAMP_MS } from './constants';
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
  const gain = sliderToGain(parseInt(volSlider.value, 10));
  updateVolumeDisplay(gain);
}

// ============================================================
// AudioContext
// ============================================================

export function ensureAudioContext(): boolean {
  if (state.actx) return true;
  const AudioContextCtor =
    window.AudioContext ||
    (window as WindowWithWebkitAudioContext).webkitAudioContext;
  if (!AudioContextCtor) return false;
  state.actx = new AudioContextCtor();
  state.gainNode = state.actx.createGain();
  state.gainNode.gain.value = sliderToGain(parseInt(volSlider.value, 10));
  state.gainNode.connect(state.actx.destination);
  return true;
}

// ============================================================
// バッファ再生
// ============================================================

function applyFadeInToBuffer(abuf: AudioBuffer, rampMs: number): void {
  const rampFrames = Math.min(
    abuf.length,
    Math.max(1, Math.floor((abuf.sampleRate * rampMs) / 1000)),
  );
  for (let c = 0; c < abuf.numberOfChannels; c++) {
    const d = abuf.getChannelData(c);
    for (let i = 0; i < rampFrames; i++) {
      d[i] *= (i + 1) / rampFrames;
    }
  }
}

export function armNextBufferRampIn(): void {
  state.nextBufferRampIn = true;
}

export function playBuffer(abuf: AudioBuffer | null): void {
  if (!abuf || !state.actx || !state.gainNode) return;
  if (state.nextBufferRampIn) {
    applyFadeInToBuffer(abuf, RESYNC_RAMP_MS);
    state.nextBufferRampIn = false;
  }
  const src = state.actx.createBufferSource();
  src.buffer = abuf;
  src.connect(state.gainNode);
  state.activeSources.add(src);
  src.onended = () => {
    state.activeSources.delete(src);
  };

  const now = state.actx.currentTime;
  if (state.nextTime < now + 0.01) state.nextTime = now + state.playbackBuffer;
  src.start(state.nextTime);
  const bufMs = Math.round((state.nextTime - state.actx.currentTime) * 1000);
  state.nextTime += abuf.duration;

  infoBuf.textContent = bufMs + ' ms';
  updateMeter(abuf.getChannelData(0));
}

// ============================================================
// PCM デコード
// ============================================================

export function updateAudioInfo(
  sampleRate: number,
  channels: number,
): void {
  infoSR.textContent = sampleRate + ' Hz';
  infoCH.textContent = channels + 'ch';
}

export function handlePcm16(
  buf: ArrayBuffer,
  sampleRate: number,
  channels: number,
  frameCount: number,
): void {
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
// VU メーター
// ============================================================

let bars: HTMLDivElement[] = [];

export function initMeter(): void {
  bars = [];
  for (let i = 0; i < NUM_BARS; i++) {
    const b = document.createElement('div');
    b.className = 'bar';
    b.style.height = '2px';
    meterEl.appendChild(b);
    bars.push(b);
  }
}

function updateMeter(samples: Float32Array): void {
  const g = state.gainNode ? state.gainNode.gain.value : 1;
  const step = Math.floor(samples.length / NUM_BARS) || 1;
  for (let i = 0; i < NUM_BARS; i++) {
    let rms = 0;
    for (let j = 0; j < step; j++) {
      const s = (samples[i * step + j] || 0) * g;
      rms += s * s;
    }
    rms = Math.sqrt(rms / step);
    const h = Math.min(100, Math.round(rms * 500));
    bars[i].style.height = Math.max(2, h) + '%';
    bars[i].style.background = state.muted
      ? '#333'
      : h > 80
        ? '#ef4444'
        : h > 50
          ? '#f59e0b'
          : '#22c55e';
  }
}
