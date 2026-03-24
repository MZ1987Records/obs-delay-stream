# 依存ライブラリとライセンス

obs-delay-stream v2.0.0 で使用しているライブラリ一覧とそのライセンスをまとめます。

## 外部ライブラリ

| ライブラリ | バージョン | 導入方法 | ライセンス | 用途 |
|---|---|---|---|---|
| **OBS Studio (libobs)** | 最新開発版 | `third_party/` に取得 | **GPL 2.0+** | プラグインフレームワーク |
| **Qt6 / Qt5 Widgets** | Qt6優先, Qt5 5.15フォールバック | CMake find_package | **LGPL 3.0** | GUIウィジェット |
| **FFmpeg** (libavcodec, libavutil, libswresample) | OBS deps同梱版 | OBS依存関係経由 | **LGPL 2.1+** | Opus エンコード/デコード、オーディオ変換 |
| **WebSocket++** | 0.8.2 | ヘッダオンリー (`third_party/websocketpp/`) | **BSD 3-Clause** | WebSocketサーバー実装 |
| **Asio** (Standalone) | 1.18.2 | ヘッダオンリー (`third_party/asio/`) | **Boost Software License 1.0** | 非同期ネットワークI/O |
| **opus-decoder** | 0.7.11 | `receiver/third_party/opus-decoder/` に同梱 | **MIT** | ブラウザOpusデコーダのWASMフォールバック |

## Windows システムライブラリ

| ライブラリ | 用途 |
|---|---|
| `ws2_32.lib` / `mswsock.lib` | Winsock2 TCP/IPネットワーキング |
| `iphlpapi.lib` | IPヘルパーAPI |
| `Shell32.lib` | シェルユーティリティ |
| `Urlmon.lib` | URLダウンロード |

## ライセンス上の注意点

- **GPL 2.0+ (OBS Studio)**: OBSプラグインとして配布する場合、プラグイン自体もGPL互換ライセンスである必要がある。このプロジェクトで使用している他のライブラリ (BSD 3-Clause, Boost 1.0, LGPL) はすべてGPL互換。
- **LGPL 2.1+/3.0 (FFmpeg, Qt)**: 動的リンクであればプロプライエタリコードとの組み合わせが可能。Qt、FFmpegともにOBS経由でDLLとして提供されるため、この条件を満たしている。
- **BSD 3-Clause (WebSocket++)** / **Boost 1.0 (Asio)**: 非常に寛容なライセンスで、著作権表示の保持が主な条件。

## ビルド要件

- CMake 3.16+ (OBS側は3.28+)
- Visual Studio 2022 + C++ Desktop Development
- C++17
- Windows 11 SDK 10.0.22621.0+
