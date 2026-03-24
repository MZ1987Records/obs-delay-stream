# obs-delay-stream  v2.0.0

OBSに音声遅延 + パフォーマー向けWebSocket配信 + IP隠蔽トンネル機能を追加するプラグインです。

---

## 機能一覧

| 機能 | 説明 |
|------|------|
| 遅延処理 ON/OFF | 音声をms単位で遅延。OFFでパススルー |
| サブCH配信 ON/OFF | パフォーマー向けWebSocket配信の個別停止 |
| 配信ID | 複数配信者のURL重複防止。半角英数字 |
| サブCH x10 | 各chに個別遅延設定。受信URL発行 |
| 同期フロー | 3ステップで全員を自動一括最適化 |
| パフォーマー遅延計測 | ping/pong RTTでサブch遅延の参考値を算出 |
| RTMP遅延計測 | ハンドシェイクRTTでマスター遅延の参考値を算出 |
| トンネル内蔵 | ngrok / cloudflared でIPを隠したURLを発行 |

---

## ビルド手順（Windows）

### 最短手順（推奨）: `build.bat` を実行

`build.bat` は **OBS Studio の自動ダウンロード・自動ビルド** と  
`websocketpp/asio` の取得、プラグインのビルドとインストールまで **一括** で行います。  
そのため通常は手動のビルド手順は不要です。

```powershell
# このREADMEがあるフォルダで実行
cd C:\obs-delay-stream
.\build.bat
```

既存の OBS ソースを使いたい場合は、パスを指定できます。

```powershell
.\build.bat D:\dev\obs-studio
```

設定は `build.env` でも指定できます（`build.env.sample` 参照）。

```
OBS_SOURCE_DIR=D:\dev\obs-studio
OBS_LEGACY_INSTALL=0
```

> **補足:** インストール先はデフォルトで `C:\ProgramData\obs-studio` です。  
> レガシー配置（`C:\Program Files\obs-studio`）に入れる場合は `OBS_LEGACY_INSTALL=1` を指定してください。  
> コピーに失敗する場合は管理者権限で実行してください。

### 必要なもの（公式準拠）

- OS: Windows 10 1909+ / Windows 11
- Visual Studio 2022 17.13.2 以上（https://visualstudio.microsoft.com/）「C++によるデスクトップ開発」、Windows 11 SDK 10.0.22621.0 以上、C++ ATL for v143、MSVC v143 を含める
- CMake 3.28 以上（https://cmake.org/download/）
- Git for Windows（https://gitforwindows.org/）
- OBS Studio ソースコード（手動ビルドの場合のみ。`build.bat` は自動取得）

---

### 手動ビルド手順（必要な場合のみ）

#### Step 1 — OBS Studioのビルド

> **重要:** `build.bat` はデフォルトで OBS Studio を **自動ダウンロード・自動ビルド** します。  
> そのため、通常はユーザが手動で OBS をビルドする必要はありません。  
> 既存の OBS ソースを使いたい場合のみ、手動ビルドや `OBS_SOURCE_DIR` の指定を行ってください。

OBS公式ドキュメント（https://github.com/obsproject/obs-studio/wiki/build-instructions-for-windows）に従ってOBSをビルドします。

```powershell
# 1. OBSソースをクローン（サブモジュール含む）
# 例: プラグイン配下の third_party に展開
git clone --recursive https://github.com/obsproject/obs-studio.git C:\obs-delay-stream\third_party\obs-studio
cd C:\obs-delay-stream\third_party\obs-studio

# 2. CMakeプリセットを確認
cmake --list-presets

# 3. Windows x64プリセットで構成
cmake --preset windows-x64

# 4. ビルド
cmake --build --preset windows-x64 --config RelWithDebInfo
```

> **Tips:** OBSのフルビルドには15〜30分かかります。
> `build_x64` フォルダが生成されれば次のステップに進めます。

---

#### Step 2 — サードパーティライブラリの取得（header-only）

```powershell
# プラグインのフォルダへ移動 (このREADMEがあるフォルダ)
cd C:\obs-delay-stream   # ← ZIPを展開した場所

# third_party フォルダ作成
mkdir third_party
cd third_party

# websocketpp (header-only WebSocketライブラリ)
git clone https://github.com/zaphoyd/websocketpp.git

# asio (Boostなしで使えるネットワークライブラリ)
git clone --branch asio-1-18-2 --depth 1 https://github.com/chriskohlhoff/asio.git

cd ..
```

完了後のフォルダ構成:
```
obs-delay-stream/
  third_party/
    websocketpp/
      websocketpp/       ← ヘッダーファイルがここに入る
    asio/
      asio/include/      ← ヘッダーファイルがここに入る
  src/
    plugin-main.cpp
    ...
  CMakeLists.txt
  README.md
```

---

#### Step 3 — プラグインのビルド

```powershell
# プラグインのフォルダで実行
cd C:\obs-delay-stream

# ビルドフォルダを作成してCMake構成
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DOBS_SOURCE_DIR=C:\obs-delay-stream\third_party\obs-studio `
  -DOBS_PLUGIN_LEGACY_INSTALL=OFF

# ビルド実行
cmake --build build --config RelWithDebInfo
```

成功すると `build\RelWithDebInfo\obs-delay-stream.dll` が生成されます。

---

#### Step 4 — OBSへのインストール

方法A: cmake --install（手動ビルド時の推奨）

```powershell
cmake --install build --config RelWithDebInfo `
  --prefix "C:\ProgramData\obs-studio"
```

方法B: 手動コピー

```
build\RelWithDebInfo\obs-delay-stream.dll
  → C:\ProgramData\obs-studio\plugins\obs-delay-stream\bin\64bit\

data\locale\en-US.ini
  → C:\ProgramData\obs-studio\plugins\obs-delay-stream\data\locale\
```

> **補足:** `C:\Program Files\obs-studio\obs-plugins\64bit` はレガシー扱いで、将来のOBSで無効化予定です。
> レガシーへ入れる場合は、CMake構成時に `-DOBS_PLUGIN_LEGACY_INSTALL=ON` を指定し、`cmake --install` の `--prefix` を `C:\Program Files\obs-studio` にしてください。

---

#### Step 5 — 動作確認

1. OBS Studioを起動
2. 音声ソース（マイク・デスクトップ音声など）を右クリック
3. **フィルター** → **＋** → **「Delay Stream (遅延 + WebSocket配信)」** を選択
4. GUIパネルが開けばインストール成功

---

## よくあるビルドエラーと対処法

### `Could not find libobs`
```
CMake Error: Could not find libobs
```
**対処:** `OBS_SOURCE_DIR` のパスが正しいか確認。OBSのビルドが完了しているか確認。

```powershell
# パスを絶対パスで指定し直す
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DOBS_SOURCE_DIR="C:\obs-studio"
```

---

### `Cannot open include file: 'websocketpp/...'`
**対処:** `third_party/websocketpp` が正しくクローンされているか確認。

```powershell
ls third_party\websocketpp\websocketpp\server.hpp
# ファイルが存在すればOK
```

---

### `LNK2019: unresolved external symbol`（リンクエラー）
**対処:** Visual Studioの **x64** ビルドになっているか確認。

```powershell
# -A x64 が指定されているか確認
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ...
```

---

### OBSに読み込まれない（フィルター一覧に表示されない）
- `.dll` のコピー先が正しいか確認（`C:\ProgramData\obs-studio\plugins\obs-delay-stream\bin\64bit\`）
- `en-US.ini` のコピー先が正しいか確認（`C:\ProgramData\obs-studio\plugins\obs-delay-stream\data\locale\`）
- OBSを管理者権限で起動してみる
- OBSのログ（`%APPDATA%\obs-studio\logs\`）でエラーを確認

---

## 使い方

### 初期設定

1. フィルターパネルを開く
2. **配信ID** を設定（例: `myshow2024`）。他の配信者と重複しない英数字

### パフォーマーへの接続案内

配布用のURL（`https://.../#!/{sid}/{ch}`）を共有し、開いてもらう（入力不要）。

### トンネル使用時（IP隠蔽）

**cloudflaredの場合（推奨・無料・認証不要）:**
1. `cloudflared.exe path` は未入力でOK（カスタム指定したい場合のみ exe のパスを入力）
2. 「トンネルを起動」ボタンを押す（デフォルトでは初回に exe が自動ダウンロードされる）
3. `https://xxxx.trycloudflare.com` 形式のURLが発行される
4. CH別URL（`https://.../#!/{sid}/{ch}`）をコピーしてパフォーマーに共有

> **注意:** セキュリティソフトが `api.trycloudflare.com` をブロックしてトンネル接続に失敗することがあります。  
> その場合は `api.trycloudflare.com` を例外（許可）に追加してください。

※ 自動ダウンロードの保存先:
`%LOCALAPPDATA%\\obs-delay-stream\\bin\\cloudflared.exe`

**ngrokの場合:**
1. https://ngrok.com でアカウント作成 → アクセストークンを取得
2. https://ngrok.com/download から `ngrok.exe` をダウンロード
3. OBSのフィルターGUIでパスとトークンを設定（自動ダウンロード非対応）
4. 「トンネルを起動」ボタンを押す

### 同期フロー（推奨手順）

1. 全パフォーマーが `receiver/index.html` に接続済みであることを確認
2. 「🔄 同期フロー開始」ボタンを押す
3. Step1: 自動計測完了後、提案値を確認して「一括反映」
4. Step3: RTMP計測完了後、マスター遅延を確認して「反映して完了」

---

## ポート

| 用途 | ポート | プロトコル |
|------|--------|-----------|
| WebSocket（全CH共有） | 19000 | TCP |

ファイアウォールで **TCP 19000** の受信を許可してください（LAN運用の場合）。
トンネル使用時はファイアウォール設定不要です。

---

## ファイル構成

```
obs-delay-stream/
  src/
    plugin-main.cpp       OBSプラグイン本体・GUI
    delay-filter.hpp      リングバッファ遅延処理
    websocket-server.hpp  WebSocketルーター（パスルーティング）
    rtmp-prober.hpp       RTMPハンドシェイク遅延計測
    sync-flow.hpp         同期フロー管理（3ステップ自動化）
    tunnel-manager.hpp    ngrok/cloudflared子プロセス管理
  third_party/            （git cloneで追加）
    websocketpp/
    asio/
  data/locale/
    en-US.ini
  receiver/
    index.html            パフォーマー受信ページ（Chrome用）
  CMakeLists.txt
  README.md
```

---

## ライセンス

MIT License
