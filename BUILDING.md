# ビルド手順（Windows）

## 最短手順（推奨）

`build.bat` は以下を一括実行します。

- OBS Studio ソースの自動取得/ビルド（未準備時）
- `websocketpp` / `asio` の取得
- プラグインの CMake 構成/ビルド
- OBS へのインストール（ProgramData 既定）

```powershell
cd C:\obs-delay-stream
.\build.bat
```

既存の OBS ソースを使う場合:

```powershell
.\build.bat D:\dev\obs-studio
```

receiver ファイルだけ更新する場合:

```powershell
.\build.bat --receiver-only
```

## build.bat の設定

`build.env`（`build.env.sample` 参照）に以下を指定できます。

```text
OBS_SOURCE_DIR=D:\dev\obs-studio
OBS_LEGACY_INSTALL=0
OBS_CI=1
```

- `OBS_SOURCE_DIR`: OBS Studio ソースのパス
- `OBS_LEGACY_INSTALL`: `1` で Program Files レイアウト、`0` で ProgramData レイアウト（既定）
- `OBS_CI`: `1` でインストール/`pause` をスキップ

既定インストール先:

- ProgramData（推奨）: `C:\ProgramData\obs-studio`
- legacy: `C:\Program Files\obs-studio`

## 必要環境

- OS: Windows 10 1909+ / Windows 11
- Visual Studio 2022（Desktop development with C++、MSVC v143、Windows SDK）
- CMake 3.16+
- Git for Windows

## 手動ビルド（必要時のみ）

### 1. OBS Studio をビルド

```powershell
git clone --recursive https://github.com/obsproject/obs-studio.git C:\obs-delay-stream\third_party\obs-studio
cd C:\obs-delay-stream\third_party\obs-studio
cmake --list-presets
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo --parallel
```

### 2. 依存ライブラリを取得

```powershell
cd C:\obs-delay-stream
mkdir third_party
cd third_party
git clone https://github.com/zaphoyd/websocketpp.git
git clone --branch asio-1-18-2 --depth 1 https://github.com/chriskohlhoff/asio.git
```

### 3. プラグインを構成/ビルド

```powershell
cd C:\obs-delay-stream
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DOBS_SOURCE_DIR=C:\obs-delay-stream\third_party\obs-studio `
  -DOBS_PLUGIN_LEGACY_INSTALL=OFF
cmake --build build --config RelWithDebInfo --parallel
```

生成物:

- `build\RelWithDebInfo\obs-delay-stream.dll`
- `build\receiver\index.html`（`@PROJECT_VERSION@` 展開済み）

### 4. OBS へインストール

推奨（CMake install）:

```powershell
cmake --install build --config RelWithDebInfo --prefix "C:\ProgramData\obs-studio"
```

legacy 配置:

```powershell
cmake --install build --config RelWithDebInfo --prefix "C:\Program Files\obs-studio"
```

手動コピー時は、DLL だけでなく `receiver` 一式も必要です。

ProgramData 配置の例:

```text
build\RelWithDebInfo\obs-delay-stream.dll
  -> C:\ProgramData\obs-studio\plugins\obs-delay-stream\bin\64bit\
data\locale\*.ini
  -> C:\ProgramData\obs-studio\plugins\obs-delay-stream\data\locale\
build\receiver\index.html
receiver\receiver.js
receiver\ui.css
receiver\*.svg
receiver\third_party\**
  -> C:\ProgramData\obs-studio\plugins\obs-delay-stream\data\receiver\
```

### 5. 動作確認

1. OBS Studio を起動
2. 音声ソースを右クリック
3. フィルター -> `+` -> `obs-delay-stream`
4. GUI が開けば完了

## よくあるエラー

### `Could not find libobs`

- `OBS_SOURCE_DIR` が OBS ソースのルートを指しているか確認
- `OBS_SOURCE_DIR\build_x64\libobs\RelWithDebInfo\obs.lib` の存在を確認

### `Cannot open include file: 'websocketpp/...'`

- `third_party\websocketpp\websocketpp\server.hpp` の存在を確認

### receiver ページの見た目が崩れる / 404

- `receiver` 配下（`receiver.js`, `ui.css`, `*.svg`, `third_party`）がインストール先にコピーされているか確認
- `index.html` は `build\receiver\index.html` を使う

## 現在の主要ファイル構成

```text
obs-delay-stream/
  .github/workflows/build.yml      リリースビルド（タグ push）
  cmake/
    embed_html.cmake               receiver HTML 埋め込みヘルパー
  data/locale/
    en-US.ini
    ja-JP.ini
  receiver/
    index.html                     受信ページテンプレート（@PROJECT_VERSION@ を展開）
    receiver.js                    受信ページロジック
    ui.css
    *.svg
    third_party/
      bulma.min.css
      fontawesome/
      opus-decoder/
  src/
    plugin-main.cpp                プラグイン本体
    constants.hpp                  共有定数
    delay-filter.hpp               遅延バッファ処理
    websocket-server.hpp           WebSocket/HTTP サーバー
    rtmp-prober.hpp                RTMP 遅延計測
    sync-flow.hpp                  同期フロー制御
    tunnel-manager.hpp             cloudflared 管理
  third_party/                     （ローカル取得。通常は Git 管理外）
    obs-studio/
    websocketpp/
    asio/
  build.bat
  build.env.sample
  CMakeLists.txt
  README.md
  BUILDING.md
```

補足:

- `build/generated/receiver_index_html.hpp` はビルド時の生成ファイルです。
- `build/receiver/index.html` は `receiver/index.html` から configure された出力です。
