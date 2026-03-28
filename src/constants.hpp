#pragma once

// 共通定数（このプロジェクトで一元管理する）

// チャンネル数・ポート
static constexpr int   MAX_SUB_CH      = 20;
static constexpr int   WS_PORT         = 19000;
static constexpr float MAX_DELAY_MS    = 10000.0f;

// WebSocket遅延計測パラメータ
static constexpr int   PING_COUNT      = 10;
static constexpr int   PING_INTV_MS    = 150;

// RTMPプローブパラメータ
static constexpr int   RTMP_PROBE_CNT  = 10;
static constexpr int   RTMP_PROBE_INTV = 300;

// ネットワーク・ソケット
static constexpr int   SOCKET_TIMEOUT_MS = 2000;

// 計測完了後のpong待機上限 (ms)
static constexpr int   PONG_WAIT_MS    = 600;

// cloudflaredトンネル管理
static constexpr int   CLOUDFLARED_URL_TIMEOUT_S  = 60;
static constexpr int   CLOUDFLARED_POLL_INTV_MS   = 500;
static constexpr int   TUNNEL_KILL_TIMEOUT_MS     = 2000;
