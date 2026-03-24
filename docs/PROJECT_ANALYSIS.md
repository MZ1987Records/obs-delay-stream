# obs-delay-stream プロジェクト分析レポート

## 1. プロジェクト概要

**obs-delay-stream** は OBS Studio 用の音声フィルタープラグイン。
ホスト（DJ/VJ）から最大10名のダンサーに、個別に遅延調整された音声を WebSocket 経由でリアルタイム配信する。
各ダンサーはブラウザ上の Web Audio API クライアントで音声を受信し、VRChat 上でのダンス同期を実現する。

- **バージョン:** 2.0.0
- **プラットフォーム:** Windows x64 専用
- **言語:** C++17
- **ビルドシステム:** CMake 3.16+ / Visual Studio 2022 (MSVC)
- **ライセンス:** GPL-2.0

---

## 2. ディレクトリ構成

```
obs-delay-stream/
├── CMakeLists.txt          # ビルド定義
├── build.bat               # ビルドスクリプト
├── release_port.bat        # ポート解放スクリプト
├── cmake/
│   └── embed_html.cmake    # receiver/index.html をビルド時にC++ヘッダへ埋め込む
├── data/
│   └── locale/
│       ├── en-US.ini       # OBSプラグイン表示名定義（英語）
│       └── ja-JP.ini       # OBSプラグイン表示名定義（日本語）
├── receiver/
│   └── index.html          # ダンサー用受信クライアント（ビルド時にDLLへ埋め込まれる）
├── src/
│   ├── plugin-main.cpp     # メインプラグインエントリ（OBS API統合、GUI、音声処理）
│   ├── websocket-server.hpp# WebSocketサーバー（パスルーティング、音声配信）
│   ├── delay-filter.hpp    # リングバッファ遅延器
│   ├── sync-flow.hpp       # 3ステップ同期フロー管理
│   ├── rtmp-prober.hpp     # RTMP RTT計測（配信遅延推定）
│   └── tunnel-manager.hpp  # ngrok/cloudflared トンネル管理
└── third_party/
    ├── websocketpp/        # WebSocket C++ ライブラリ（ヘッダオンリー）
    └── asio/               # ASIO スタンドアロン（Boostなし）
```

---

## 3. 技術スタック

### 3.1 OBS Plugin API

- **プラグイン種別:** `OBS_SOURCE_TYPE_FILTER` (音声フィルター)
- OBSの音声パイプラインに挿入され、`ds_filter_audio()` が各音声フレームごとに呼ばれる
- OBSのプロパティシステム (`obs_properties_t`) を使用してGUIを構築
- VSTプラグインではなく、OBS専用のネイティブプラグインAPI

### 3.2 WebSocket (websocketpp + ASIO Standalone)

- **websocketpp**: C++11対応のヘッダオンリーWebSocketライブラリ
- **ASIO Standalone**: Boost不要の非同期I/Oライブラリ
- `ASIO_STANDALONE` と `_WEBSOCKETPP_CPP11_STL_` を定義してBoostフリーで使用
- 単一ポート (19000) でパスルーティング: `ws://[IP]:19000/[配信ID]/[ch番号]`

### 3.3 音声処理

- **形式（内部処理）:** Interleaved float32 PCM
- **WebSocket配信:** PCM16（フォールバック） / Opus
- **サンプルレート:** 48kHz (OBSデフォルト)
- **チャンネル数:** 2 (ステレオ)
- **遅延処理:** リングバッファ方式 (`DelayBuffer` クラス)
- **最大遅延:** 10,000ms

### 3.4 Windows API

- **Winsock2:** WebSocket通信、RTMP計測、ngrok API取得
- **CreateProcess/TerminateProcess:** ngrok/cloudflared 子プロセス管理
- **GetAdaptersInfo:** ローカルIP自動検出
- **Clipboard API:** URL のクリップボードコピー

### 3.5 サードパーティ依存

| ライブラリ | バージョン | 用途 | 同梱方式 |
|-----------|-----------|------|---------|
| websocketpp | - | WebSocket サーバー | third_party/ にヘッダ同梱 |
| ASIO Standalone | - | 非同期I/O | third_party/ にヘッダ同梱 |
| OBS libobs | - | OBS プラグインAPI | 外部参照 (OBS_SOURCE_DIR) |
| FFmpeg (avcodec/avutil/swresample) | - | Opus エンコード・リサンプル | OBS deps 同梱 |
| ws2_32/mswsock/iphlpapi | Windows SDK | ネットワーク | システムライブラリ |

---

## 4. 機能アーキテクチャ

### 4.1 音声配信フロー

```
OBS Audio Pipeline
    │
    ▼
ds_filter_audio()  [Audio Thread, ~48kHz]
    │
    ├──► master DelayBuffer.process()    → マスター遅延適用
    │
    └──► for each sub-ch (0-9):
         ├── sub[i] DelayBuffer.process() → 個別遅延適用
         └── router.send_audio(i, ...)    → WebSocket送信
```

### 4.2 WebSocket バイナリプロトコル

```
PCM16:
[4B magic: 0x41554449 "AUDI"]
[4B sample_rate: uint32]
[4B channels: uint32]
[4B frames: uint32]
[int16 × frames × channels: PCM data]

OPUS:
[4B magic: 0x4F505553 "OPUS"]
[4B sample_rate: uint32]
[4B channels: uint32]
[4B frames: uint32]
[Opus packet bytes]
```

### 4.3 WebSocket JSON 制御メッセージ

| 方向 | type | 内容 |
|------|------|------|
| OBS→Browser | `ping` | `{"type":"ping","seq":N,"t":T}` |
| Browser→OBS | `pong` | `{"type":"pong","seq":N}` |
| OBS→Browser | `latency_result` | 計測結果 (avg_rtt, one_way, min, max, samples) |
| OBS→Browser | `apply_delay` | 遅延設定値 `{"ms":X}` |
| OBS→Browser | `session_info` | 接続情報 `{"stream_id":"xxx","ch":N}` |

### 4.4 3ステップ同期フロー (`SyncFlow`)

VRChat上での全員同期を実現するための自動遅延計算:

| ステップ | 処理 | 目的 |
|----------|------|------|
| Step1 | 全接続CHに並列ping (10回) | 各ダンサーまでの片道遅延を測定 |
| Step2 | `max_one_way - ch_one_way` を各CHに適用 | 遅い側に合わせて全CHの遅延を均一化 |
| Step3 | RTMP RTT計測 | 配信遅延を加味してマスター遅延を算出 |

**設計思想:** 「遅い側に合わせる」
- 各サブch遅延 = `max_one_way - ch_one_way` (差分を追加)
- マスター遅延 = `max_one_way + rtmp_one_way` (パフォーマー基準 + 配信遅延)

### 4.5 RTMP 計測 (`RtmpProber`)

配信サーバーまでのネットワーク遅延を推定:

1. TCP SYN → SYN-ACK (TCP RTT)
2. RTMP C0+C1 送信 → S0+S1 受信 (RTMP handshake RTT)
3. N回繰り返して平均・最大・最小・ジッターを算出
4. RTT ÷ 2 で片道遅延を推定

### 4.6 トンネル管理 (`TunnelManager`)

リモートダンサー向けにパブリックURLを提供:

| ツール | URL取得方法 | 結果 |
|--------|-----------|------|
| ngrok | `http://127.0.0.1:4040/api/tunnels` ポーリング | `tcp://X.tcp.ngrok.io:NNNNN` → `ws://` |
| cloudflared | stderr に出力される URL を捕捉 | `https://xxx.trycloudflare.com` → `wss://` |

### 4.7 受信クライアント (`receiver/index.html`)

ブラウザ上で動作する完全自己完結型 HTML5 アプリケーション:

- **Web Audio API:** `AudioContext` + `AudioBufferSourceNode` でリアルタイム再生
- **WebSocket:** バイナリフレームからPCM16/Opusデコード
- **VUメーター:** リアルタイム音量表示
- **ボリュームコントロール:** dB単位での調整
- **遅延計測:** ping/pong による RTT 表示
- **ダークテーマ:** レスポンシブデザイン
- **DLL内蔵配信:** ビルド時に `cmake/embed_html.cmake` でC++ヘッダへ変換し、WebSocketサーバーが `/receiver` パスで直接配信する（外部ファイル不要）

---

## 5. プロセス・スレッドモデル

すべて **OBS Studio の単一プロセス内** で動作する。

| # | スレッド | 生成元 | 用途 | 終了管理 |
|---|---------|--------|------|---------|
| 1 | OBS Audio Thread | OBS本体 | `ds_filter_audio()` 音声処理+送信 | OBS管理 |
| 2 | OBS UI Thread | OBS本体 | GUI描画、設定変更、ボタンCB | OBS管理 |
| 3 | WebSocket ASIO Thread | `StreamRouter::start()` | `server::run()` イベントループ | `stop()` で join |
| 4 | Measurement Thread ×N | `StreamRouter::start_measurement()` | ping/pong RTT計測 | ※後述 |
| 5 | RTMP Prober Thread | `RtmpProber::start()` | RTMP RTT計測 | `cancel()` で join |
| 6 | Tunnel Worker Thread | `TunnelManager::start()` | 子プロセス管理 | `stop()` で join |

※ スレッド4は修正前は `.detach()` で生成されライフサイクル管理がなかった（CRITICAL-2 として修正済み）

---

## 6. GUIの構成

OBSのプロパティシステムで構築された設定パネル:

- **基本設定:** フィルター有効/無効、WebSocket有効/無効、配信ID
- **マスター遅延:** 全体に適用する遅延値 (ms)
- **CH 1-10:** 各チャンネルの個別遅延、接続状態表示、ping計測ボタン、URL表示・コピー
- **同期フロー:** 3ステップの自動遅延計算 (開始/適用/確認ボタン)
- **RTMP計測:** 配信サーバーへのRTT計測
- **トンネル:** ngrok/cloudflared によるパブリックURL取得

---

## 7. ビルド手順

詳細は [docs/BUILDING.md](BUILDING.md) を参照。

---

## 8. 既知の設計上の考慮点

1. **HTML受信クライアントのセキュリティ:** ローカルに保存したHTMLファイルから `ws://` に接続するため、一部のセキュリティソフトが不審な動作として検知する。本来はWebSocketサーバーと同一サービスから固定HTMLを配信すべき。

2. **単一プロセスアーキテクチャ:** すべてがOBSプロセス内で動作するため、WebSocketサーバーやトンネルプロセスの問題がOBS全体に影響する。

3. **Windows専用:** Winsock2、CreateProcess 等の Windows API に依存しており、クロスプラットフォーム対応には大幅な書き換えが必要。
