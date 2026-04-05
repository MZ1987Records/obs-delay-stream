import { Emitter } from './emitter';
import type { LatencyResultMessage } from './types';

export type AppEvents = {
  /** 接続前バリデーション失敗 */
  'connect:rejected': {
    reason: 'no-audio' | 'no-sid' | 'invalid-sid' | 'no-code' | 'ws-failed';
  };
  /** 接続開始（WebSocket 生成済み） */
  'ws:connecting': { url: string };
  /** WebSocket 接続確立 */
  'ws:open': { url: string };
  /** WebSocket 切断 */
  'ws:close': {
    code?: number;
    reason?: string;
    cause: 'user' | 'timeout' | 'error' | 'server';
  };
  /** session_info 受信 */
  'ctrl:session': {
    streamId?: string;
    ch?: number | string;
    code?: string;
    memo?: unknown;
  };
  /** memo 受信 */
  'ctrl:memo': { memo: unknown };
  /** ping 受信 — 計測進行 */
  'ctrl:ping': { count: number };
  /** レイテンシ計測結果 */
  'ctrl:latency': LatencyResultMessage;
  /** 遅延適用通知 */
  'ctrl:delay': { ms: number | string; reason: string };
};

export const bus = new Emitter<AppEvents>();
