# obs-delay-stream  v7.1.0

**VRChat Dancer/音楽パフォーマー支援ツール[obs-delay-stream]**

![Windows](https://img.shields.io/badge/platform-Windows-blue)
![License: GPL-2.0-or-later](https://img.shields.io/badge/License-GPL--2.0--or--later-blue)
![OSS: OBS Studio](https://img.shields.io/badge/OSS-OBS%20Studio-lightgrey)
![OSS: WebSocket++](https://img.shields.io/badge/OSS-WebSocket++-lightgrey)
![OSS: FFmpeg](https://img.shields.io/badge/OSS-FFmpeg-lightgrey)

[BOOTH](https://mz1987records.booth.pm/items/8134637) | [バグ報告](https://github.com/MZ1987Records/obs-delay-stream/issues/new/choose) | [English](README-en.md)

<p align="center">
  <img src="receiver/images/obs-delay-stream-logo.svg" alt="obs-delay-stream logo" width="400">
</p>

ダンサー間の同期ズレやダンスとワールド音楽の同期ズレを、OBSプラグインとGoogle Chromeのみで自動計測・自動調整し解決するプラグインです。OBSに出演者向けWebSocket音声配信機能を追加します。
同時接続出演者数20人まで。

各出演者への遅延時間が自動計測され、タイミングが揃うように自動調整された音声ストリームをGoogle Chromeブラウザで受信可能です。
OBS配信（RTSP E2E）の遅延計測にも対応し、ワールドへ配信される音声との同期調整も行えます。
IP隠蔽トンネル機能つき。

- SYNCROOMやDAWを必要としません。
- 出演者は配信者から渡された受信用URLにGoogle Chromeでアクセスするだけで、低遅延かつ同期された音声を受信できます。
- VRChatクライアントに影響を与えません。
- 受信ページは音量調整・リシンク/自動リシンク・スペクトラムメーター・JP/EN言語表示に対応しています。

---

## 設定タブ一覧

| タブ | 内容 |
|------|------|
| 出演者名 | 出演者ごとの名前を管理します。チャンネルの追加・削除・並べ替えができます |
| トンネル | cloudflared で公開URLを発行します。IPを直接公開せずに外部共有できます |
| WS配信 | 音声コーデック（Opus / PCM）の設定、WebSocketサーバーの起動・停止と送信制御を行います |
| URL共有 | 出演者用URLの一覧表示・一括コピーができます。共有時の手間や配布ミスを減らせます |
| WS計測 | 接続中の全チャンネルのWebSocket配信遅延を自動計測します。出演者接続時の自動計測にも対応しています |
| RTSP計測 | OBS配信（RTSP E2E）の遅延を計測します。サイレント / ミックスの2つの計測モードを選べます |
| 微調整 | チャンネル別の環境遅延設定とディレイ全体図の確認ができます。想定アバター遅延の設定もここで行います |

---

## インストール

1. [Releases](https://github.com/MZ1987Records/obs-delay-stream/releases) または [BOOTH](https://mz1987records.booth.pm/items/8134637) から最新の `obs-delay-stream-vX.X.X.zip` をダウンロードして解凍
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
3. **フィルター** → **＋** → **「obs-delay-stream」** を選択
4. GUIパネルが開けばインストール成功

---

## 使い方

### 初期設定

1. フィルターパネルを開く
2. **出演者名** タブで各出演者の名前を入力
3. **WS配信** タブで **WebSocket サーバー起動** ボタンを押す

### トンネル使用時（IP隠蔽・推奨）

1. `cloudflared.exe path` は `auto` のままでOK（カスタム指定したい場合のみ exe のパスを入力）
2. 「トンネルを起動」ボタンを押す（デフォルトでは初回に exe が自動ダウンロードされる）
3. `https://xxxx.trycloudflare.com` 形式のURLが発行される

> **注意:** セキュリティソフトが `*.trycloudflare.com` をブロックしてトンネル接続に失敗することがあります。
> その場合は `*.trycloudflare.com` を例外（許可）に追加してください。

※ 自動ダウンロードされた exe の保存先:
`%LOCALAPPDATA%\obs-delay-stream\bin\cloudflared.exe`

### 出演者への接続案内

**URL共有** タブで **出演者用URL一覧をコピー** ボタンを押し、Discordなどにペーストして共有する。
各出演者に、対応する自分のURLをGoogle Chromeで開いてもらう。

### 遅延計測（推奨手順）

1. 全出演者が受信ページに接続済みであることを確認
2. **WS計測** タブで全チャンネルの遅延を計測（出演者接続時の自動計測も利用可能）
3. 計測完了後、各チャンネルのベース遅延が自動適用される
4. **RTSP計測** タブでOBS配信遅延を計測（OBSの配信を事前に開始しておく）
5. 必要に応じて **微調整** タブで環境遅延やアバター遅延を設定

---

## 開発者向け情報

ビルド手順・トラブルシューティング・ファイル構成については [BUILDING.md](BUILDING.md) を参照してください。

---

## ライセンス

- [GNU General Public License v2.0 or later](LICENSE)
- サードパーティライセンスについては [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES) を参照してください。
