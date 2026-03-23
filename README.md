# obs-delay-stream  v1.7.0

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

### 必要なもの

| ツール | バージョン | 入手先 |
|--------|-----------|--------|
| Visual Studio 2022 | Community以上 | https://visualstudio.microsoft.com/ |
| CMake | 3.16以上 | https://cmake.org/download/ |
| Git | 最新 | https://git-scm.com/ |
| OBS Studio ソースコード | 最新安定版 | https://github.com/obsproject/obs-studio |

Visual Studioのインストール時に **「C++によるデスクトップ開発」** ワークロードを必ず選択してください。

---

### Step 1 — OBS Studioのビルド

OBS公式ドキュメント（https://github.com/obsproject/obs-studio/wiki/build-instructions-for-windows）に従ってOBSをビルドします。

```powershell
# 1. OBSソースをクローン
git clone --recursive https://github.com/obsproject/obs-studio.git C:\obs-studio
cd C:\obs-studio

# 2. OBS公式の依存関係をダウンロード (初回のみ・数分かかります)
.github\scripts\build-windows.ps1 -BuildArch x64 -SkipBuild
# ↑ 依存関係だけダウンロードするスクリプト。OBS本体のビルドは次のステップで行います

# 3. CMakeでOBSを構成 + ビルド
cmake -S . -B build_x64 -G "Visual Studio 17 2022" -A x64 `
  -DENABLE_BROWSER=OFF `
  -DENABLE_VST=OFF
cmake --build build_x64 --config RelWithDebInfo --parallel
```

> **Tips:** OBSのフルビルドには15〜30分かかります。
> `build_x64` フォルダが生成されれば次のステップに進めます。

---

### Step 2 — サードパーティライブラリの取得（header-only）

```powershell
# プラグインのフォルダへ移動 (このREADMEがあるフォルダ)
cd C:\obs-delay-stream   # ← ZIPを展開した場所

# third_party フォルダ作成
mkdir third_party
cd third_party

# websocketpp (header-only WebSocketライブラリ)
git clone https://github.com/zaphoyd/websocketpp.git

# asio (Boostなしで使えるネットワークライブラリ)
git clone https://github.com/chriskohlhoff/asio.git

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

### Step 3 — プラグインのビルド

```powershell
# プラグインのフォルダで実行
cd C:\obs-delay-stream

# ビルドフォルダを作成してCMake構成
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DOBS_SOURCE_DIR=C:\obs-studio

# ビルド実行
cmake --build build --config RelWithDebInfo
```

成功すると `build\RelWithDebInfo\obs-delay-stream.dll` が生成されます。

---

### Step 4 — OBSへのインストール

#### 方法A: cmake --install（推奨）

```powershell
cmake --install build --config RelWithDebInfo `
  --prefix "C:\Program Files\obs-studio"
```

#### 方法B: 手動コピー

```
build\RelWithDebInfo\obs-delay-stream.dll
  → C:\Program Files\obs-studio\obs-plugins\64bit\

data\locale\en-US.ini
  → C:\Program Files\obs-studio\data\obs-plugins\obs-delay-stream\locale\
```

---

### Step 5 — 動作確認

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
- `.dll` のコピー先が正しいか確認（`obs-plugins\64bit\` フォルダ）
- `en-US.ini` のコピー先が正しいか確認
- OBSを管理者権限で起動してみる
- OBSのログ（`%APPDATA%\obs-studio\logs\`）でエラーを確認

---

## 使い方

### 初期設定

1. フィルターパネルを開く
2. **配信ID** を設定（例: `myshow2024`）。他の配信者と重複しない英数字
3. **IPアドレス** が自動取得されていることを確認（必要なら手動修正）

### パフォーマーへの接続案内

`receiver/index.html` をChromeで開き、以下を入力してもらう:
- IPアドレス（またはトンネルURL）
- 配信ID
- 担当CH番号（1〜10）

### トンネル使用時（IP隠蔽）

**cloudflaredの場合（推奨・無料・認証不要）:**
1. https://github.com/cloudflare/cloudflared/releases から `cloudflared-windows-amd64.exe` をダウンロード
2. OBSのフィルターGUIで `cloudflared.exe` のパスを設定
3. 「トンネルを起動」ボタンを押す
4. `wss://xxxx.trycloudflare.com` 形式のURLが発行される
5. CH別URLをコピーしてパフォーマーに共有

**ngrokの場合:**
1. https://ngrok.com でアカウント作成 → アクセストークンを取得
2. https://ngrok.com/download から `ngrok.exe` をダウンロード
3. OBSのフィルターGUIでパスとトークンを設定
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
