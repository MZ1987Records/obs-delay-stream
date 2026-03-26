# obs-delay-stream  v2.0.0

OBSに音声遅延 + パフォーマー向けWebSocket配信 + IP隠蔽トンネル機能を追加するプラグインです。

<p align="center">
  <img src="receiver/obs-delay-stream-logo.svg" alt="obs-delay-stream logo" width="280">
</p>

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

## インストール

1. [Releases](https://github.com/MZ1987Records/obs-delay-stream/releases) から最新の `obs-delay-stream-vX.X.X.zip` をダウンロードして解凍
2. ZIP内に `For ProgramData` と `For Program Files (legacy)` の2種類が入っています。使用中のOBS配置に合わせて選択してください

### ProgramData 配置（推奨）

1. `For ProgramData/plugins/obs-delay-stream` を以下へ配置:

```
C:\ProgramData\obs-studio\plugins\
```

2. OBS Studio を再起動

### Program Files 配置（レガシー）

1. `For Program Files (legacy)/obs-plugins/64bit/obs-delay-stream.dll` を以下へ配置:

```
C:\Program Files\obs-studio\obs-plugins\64bit\
```

2. `For Program Files (legacy)/data/obs-plugins/obs-delay-stream` を以下へ配置:

```
C:\Program Files\obs-studio\data\obs-plugins\
```

3. 既存ファイルがある場合は上書きでOKです（更新の場合）
4. OBS Studio を再起動（管理者権限が必要な場合があります）

### 動作確認

1. OBS Studio を起動
2. 音声ソース（マイク・デスクトップ音声など）を右クリック
3. **フィルター** → **＋** → **「Delay Stream (遅延 + WebSocket配信)」** を選択
4. GUIパネルが開けばインストール成功

---

## 使い方

### 初期設定

1. フィルターパネルを開く
2. **配信ID** を設定（例: `myshow2024`）

### パフォーマーへの接続案内

配布用のURL（`https://.../#!/{sid}/{ch}`）を共有し、開いてもらう。

### トンネル使用時（IP隠蔽）

**cloudflaredの場合（推奨・無料・認証不要）:**
1. `cloudflared.exe path` は未入力でOK（カスタム指定したい場合のみ exe のパスを入力）
2. 「トンネルを起動」ボタンを押す（デフォルトでは初回に exe が自動ダウンロードされる）
3. `https://xxxx.trycloudflare.com` 形式のURLが発行される
4. CH別URL（`https://.../#!/{sid}/{ch}`）をコピーしてパフォーマーに共有

> **注意:** セキュリティソフトが `*.trycloudflare.com` をブロックしてトンネル接続に失敗することがあります。
> その場合は `*.trycloudflare.com` を例外（許可）に追加してください。

※ 自動ダウンロードの保存先:
`%LOCALAPPDATA%\obs-delay-stream\bin\cloudflared.exe`

**ngrokの場合:**
1. https://ngrok.com でアカウント作成 → アクセストークンを取得
2. https://ngrok.com/download から `ngrok.exe` をダウンロード
3. OBSのフィルターGUIでパスとトークンを設定（自動ダウンロード非対応）
4. 「トンネルを起動」ボタンを押す

### 同期フロー（推奨手順）

1. 全パフォーマーが受信ページに接続済みであることを確認
2. 「同期フロー開始」ボタンを押す
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

## 開発者向け情報

ビルド手順・トラブルシューティング・ファイル構成については [BUILDING.md](BUILDING.md) を参照してください。

---

## ライセンス

[GNU General Public License v2.0 or later](LICENSE) — サードパーティライセンスについては [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES) を参照してください。
