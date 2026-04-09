#pragma once

#include <cstdint>

/**
 * 遅延バッファ・共通定数など、プラグイン全体で共有するコアユーティリティ。
 */
namespace ods::core {

	// チャンネル数・ポート
	static constexpr int   MAX_SUB_CH                 = 20;       ///< サブチャンネルの最大数
	static constexpr int   SUB_MEMO_MAX_CHARS         = 64;       ///< 出演者名(メモ)入力の最大文字数
	static constexpr int   WS_PORT                    = 19000;    ///< WebSocket サーバーの待受ポート
	static constexpr float MAX_DELAY_MS               = 10000.0f; ///< 遅延バッファの上限 (ms)
	static constexpr int   PLAYBACK_BUFFER_MIN_MS     = 20;       ///< 再生バッファの最小値 (ms)
	static constexpr int   PLAYBACK_BUFFER_MAX_MS     = 500;      ///< 再生バッファの最大値 (ms)
	static constexpr int   PLAYBACK_BUFFER_DEFAULT_MS = 120;      ///< 再生バッファのデフォルト値 (ms)

	// WebSocket レイテンシ計測パラメータ
	static constexpr int DEFAULT_PING_COUNT = 30;  ///< 1 回の計測で送信する ping 数
	static constexpr int PING_INTV_MS       = 150; ///< ping 送信間隔 (ms)

	// RTMP プローブパラメータ
	static constexpr int RTMP_PROBE_CNT  = 10;  ///< プローブ試行回数
	static constexpr int RTMP_PROBE_INTV = 300; ///< プローブ試行間隔 (ms)

	// RTSP E2E 計測パラメータ
	static constexpr int     RTSP_E2E_TIMEOUT_S            = 60;    ///< インパルス検出タイムアウト (s)
	static constexpr int     RTSP_E2E_READY_TIMEOUT_S      = 15;    ///< RTSP 受信開始待機タイムアウト (s)
	static constexpr int     RTSP_E2E_MEASURE_SETS_DEFAULT = 5;     ///< 接続・計測・切断のデフォルト反復回数
	static constexpr int     RTSP_IMPULSE_SAMPLES          = 256;   ///< インパルス長 (sample/ch)
	static constexpr float   RTSP_IMPULSE_AMP              = 0.9f;  ///< インパルス振幅
	static constexpr int16_t RTSP_DETECT_THRESHOLD         = 28000; ///< 検出閾値 (int16)

	// ネットワーク・ソケット
	static constexpr int SOCKET_TIMEOUT_MS = 2000; ///< TCP 接続タイムアウト (ms)
	static constexpr int PONG_WAIT_MS      = 600;  ///< 全 ping 送信後に最後の pong を待つ上限 (ms)

	// cloudflared トンネル管理
	static constexpr int CLOUDFLARED_URL_TIMEOUT_S = 60;   ///< cloudflared が URL を出力するまでの待機上限 (s)
	static constexpr int CLOUDFLARED_POLL_INTV_MS  = 500;  ///< URL 確認のポーリング間隔 (ms)
	static constexpr int TUNNEL_KILL_TIMEOUT_MS    = 2000; ///< トンネルプロセス終了待機のタイムアウト (ms)

	// UI 色（操作ボタン）
	static constexpr const char *UI_COLOR_START_BUTTON_BG = "#1E7F34"; ///< 開始ボタンの背景色
	static constexpr const char *UI_COLOR_STOP_BUTTON_BG  = "#8B1E1E"; ///< 停止ボタンの背景色
	static constexpr const char *UI_COLOR_BUTTON_TEXT     = "#FFFFFF"; ///< ボタンテキストの色

} // namespace ods::core
