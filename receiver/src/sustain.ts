import { state } from './state';
import { SUSTAIN_TAIL_MS, SUSTAIN_MAX_MS } from './constants';

// ============================================================
// サステイン（ピンポンループ）
// ============================================================

function buildPingPongBuffer(src: AudioBuffer): AudioBuffer | null {
  if (!state.actx) return null;
  const tailFrames = Math.min(
    src.length,
    Math.max(2, Math.floor(src.sampleRate * SUSTAIN_TAIL_MS / 1000)),
  );
  const startFrame = src.length - tailFrames;
  // 折り返し点でサンプル重複を除去: forward(全) + reverse(両端除外)
  const loopFrames = tailFrames * 2 - 2;
  if (loopFrames < 2) return null;
  const buf = state.actx.createBuffer(
    src.numberOfChannels, loopFrames, src.sampleRate,
  );
  for (let c = 0; c < src.numberOfChannels; c++) {
    const s = src.getChannelData(c);
    const d = buf.getChannelData(c);
    for (let i = 0; i < tailFrames; i++) {
      d[i] = s[startFrame + i];                     // forward
    }
    for (let i = 1; i < tailFrames - 1; i++) {
      d[tailFrames + i - 1] = s[startFrame + tailFrames - 1 - i]; // reverse（端点除外）
    }
  }
  return buf;
}

/**
 * 指定ノードにサステインソースをスケジュールする。
 * fadeOut=true (既定) ではフェードアウトしながらループ再生。
 * fadeOut=false ではフルボリュームで再生し、外部のクロスフェードに委ねる。
 */
export function scheduleSustain(
  dest: AudioNode,
  startTime: number,
  durationSec: number,
  fadeOut = true,
): void {
  if (!state.actx || !state.lastBuffer) return;
  const ppBuf = buildPingPongBuffer(state.lastBuffer);
  if (!ppBuf) return;
  const dur = Math.min(durationSec, SUSTAIN_MAX_MS / 1000);
  if (dur <= 0) return;

  const src = state.actx.createBufferSource();
  src.buffer = ppBuf;
  src.loop = true;

  if (fadeOut) {
    const env = state.actx.createGain();
    env.gain.setValueAtTime(1, startTime);
    env.gain.exponentialRampToValueAtTime(0.001, startTime + dur);
    env.connect(dest);
    src.connect(env);
  } else {
    src.connect(dest);
  }

  src.start(startTime);
  src.stop(startTime + dur);
}
