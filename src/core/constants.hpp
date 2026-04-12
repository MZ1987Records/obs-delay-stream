#pragma once

#include <cstdint>

/**
 * ディレイバッファ・共通定数など、プラグイン全体で共有するコアユーティリティ。
 */
namespace ods::core {

	// チャンネル数・ポート
	static constexpr int   MAX_SUB_CH                 = 20;       ///< サブチャンネルの最大数
	static constexpr int   SUB_MEMO_MAX_CHARS         = 64;       ///< 出演者名(メモ)入力の最大文字数
	static constexpr int   WS_PORT                    = 19000;    ///< WebSocket サーバーの待受ポート
	static constexpr float MAX_DELAY_MS               = 10000.0f; ///< ディレイバッファの上限 (ms)
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
	static constexpr int RTSP_E2E_TIMEOUT_S            = 60; ///< プローブ検出タイムアウト (s)
	static constexpr int RTSP_E2E_READY_TIMEOUT_S      = 15; ///< RTSP 受信開始待機タイムアウト (s)
	static constexpr int RTSP_E2E_MEASURE_SETS_DEFAULT = 5;  ///< 接続・計測・切断のデフォルト反復回数
	// ネットワーク・ソケット
	static constexpr int SOCKET_TIMEOUT_MS = 2000; ///< TCP 接続タイムアウト (ms)
	static constexpr int PONG_WAIT_MS      = 600;  ///< 全 ping 送信後に最後の pong を待つ上限 (ms)

	// cloudflared トンネル管理
	static constexpr int CLOUDFLARED_URL_TIMEOUT_S = 60;   ///< cloudflared が URL を出力するまでの待機上限 (s)
	static constexpr int CLOUDFLARED_POLL_INTV_MS  = 500;  ///< URL 確認のポーリング間隔 (ms)
	static constexpr int TUNNEL_KILL_TIMEOUT_MS    = 2000; ///< トンネルプロセス終了待機のタイムアウト (ms)

	// UI 色（ステータスインジケーター）
	static constexpr const char *UI_COLOR_STATUS_DOT_OK   = "#22CC44"; ///< 起動中・正常ステータスの丸アイコン色
	static constexpr const char *UI_COLOR_STATUS_DOT_BUSY = "#FFAA00"; ///< 開始中・処理中ステータスの丸アイコン色
	static constexpr const char *UI_COLOR_STATUS_DOT_OFF  = "#888888"; ///< 停止中ステータスの丸アイコン色

} // namespace ods::core
