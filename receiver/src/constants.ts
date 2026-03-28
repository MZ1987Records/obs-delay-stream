/** オーディオバッファの先読み秒数 */
export const AHEAD = 0.12;

/** PCM16パケットのマジックナンバー (0x41554449 = "AUDI") */
export const MAGIC_AUDI = 0x41554449;

/** Opusパケットのマジックナンバー (0x4f505553 = "OPUS") */
export const MAGIC_OPUS = 0x4f505553;

/** 最小CH番号 */
export const MIN_CH = 1;

/** デフォルト最大CH番号 */
export const DEFAULT_MAX_CH = 20;

/** 遅延計測のping回数 */
export const PING_SAMPLES = 10;

/** WebSocket接続タイムアウト (ms) */
export const CONNECT_TIMEOUT_MS = 7000;

/** VUメーターのバー数 */
export const NUM_BARS = 20;

/** デフォルトWebSocketポート番号 */
export const WS_PORT = 19000;

/** WASM連続エラーのリセット間隔 (ms) */
export const WASM_ERROR_STREAK_WINDOW_MS = 2000;

/** WASM連続エラーでフェイルオーバーするまでの回数 */
export const WASM_ERROR_STREAK_THRESHOLD = 3;

/** 再同期メッセージ表示時間 (ms) */
export const RESYNC_DISPLAY_MS = 500;

/** デフォルト音量 (0–100 スライダー値) */
export const DEFAULT_VOLUME = 84;
