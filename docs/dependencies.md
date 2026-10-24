# 依存物一覧

`docs/security-hardening-plan.md` の P0「現物把握」のうち、リポジトリから確認できる範囲をまとめる。利用者の PC に実際に置かれている exe の版・ハッシュ・署名は含まない（P0 の残作業）。

確認日: 2026-10-24。版を更新したら、この一覧と確認日を併せて更新する。

## プラグインが実行時に取得する実行ファイル

自動（Auto）モードでは、承認一覧 `src/core/approved-binaries.hpp` の版だけを取得し、サイズと SHA-256 が一致したものだけを配置・起動する。起動のたびに配置済みの exe も照合する。PATH・手動指定の exe は検証しない。

| 依存物 | 承認版 | 取得元 | SHA-256 | 追跡 |
|---|---|---|---|---|
| cloudflared | 2026.10.3（2026-10-21 公開） | `cloudflare/cloudflared` の Release `2026.10.3` の `cloudflared-windows-amd64.exe`（55,329,336 bytes） | `76b2805253d0d6ffacb092a7acfbee99e7af6917835c0c5410e23f3ef5052da2`（リリースノートと GitHub API の digest で照合） | 手動。Cloudflare の告知とリリースを確認する |
| FFmpeg | n9.0.2-17-g2a571b6068（9.0 系、LGPL、静的ビルド） | `BtbN/FFmpeg-Builds` の Release `autobuild-2026-09-30-13-08` の `ffmpeg-n9.0.2-17-g2a571b6068-win64-lgpl-9.0.zip`（171,535,354 bytes） | ZIP `6b264b9e6019103f601d98c292bd332fd87acf1c5e941ddff4fb71760fe63432`、`ffmpeg.exe`（134,497,280 bytes）`0e482b853f2fd4821055d20cdce510f0eea6027037c04b994f454a2136e8868e` | 手動。BtbN の月末ビルドは約 2 年保持される。取得できなくなったら新しい月末ビルドを承認し直す（自前では再配布しない） |

配置先: `%LOCALAPPDATA%\obs-delay-stream\bin\cloudflared\<版>\cloudflared.exe`、`%LOCALAPPDATA%\obs-delay-stream\bin\ffmpeg\<展開ディレクトリ>\bin\ffmpeg.exe`。以前の版が `latest` を自動取得した `bin\cloudflared.exe` と `bin\ffmpeg\ffmpeg-master-latest-win64-gpl\` は使わない。設定がこれらを指していれば、読込時に自動へ戻す。

取得・検証処理: `src/core/binary-verify.cpp`、`src/tunnel/tunnel-manager.cpp`（cloudflared）、`src/network/rtsp-e2e-prober.cpp`（FFmpeg）。

## ビルド時に取得してリンクするライブラリ

| 依存物 | 版 | コミット | 固定方法 | 追跡 |
|---|---|---|---|---|
| WebSocket++ | 0.8.2 以降の master | `4dfe1be74e684acca19ac1cf96cce0df9eac2a2d` | `THIRD_PARTY_REFS` | 手動。リリースは 0.8.2（2020 年）が最後 |
| libopus | 1.6.1 | `22244de5a79bd1d6d623c32e72bf1954b56235be` | `THIRD_PARTY_REFS` | 手動 |
| Asio（Standalone） | 1.18.2 | `a4d820dd69b37fb8daee275d20eb162054453414` | タグ `asio-1-18-2`（コミットは未検証） | 手動。最新は 1.38.2 だが、WebSocket++ が使う旧 API が削除されているため単純には更新できない |

取得処理: `build.bat`、`.github/workflows/build.yml`、`.github/workflows/build-test.yml`。

## OBS Studio

| 項目 | 内容 |
|---|---|
| ビルドに使う版（API の下限） | `OBS_STUDIO_REF` の 32.0.4 |
| 実行時 | 利用者がインストールした OBS の libobs と Qt を使う。プラグインは OBS 同梱の FFmpeg などをリンクせず、配布もしない |
| 最新 | 33.0.0 / 32.2.2（下限を上げるかはサポート方針として判断する） |

## 受信ページ（receiver）

配布する受信ページに含まれるもの:

| 依存物 | 版 | 入手方法 | 追跡 |
|---|---|---|---|
| @fortawesome/fontawesome-free | 7.3.1 | npm（`receiver/package-lock.json`） | Dependabot |
| bulma | 1.0.4 | npm | Dependabot |
| i18next | 26.3.6 | npm | Dependabot |
| opus-decoder（wasm-audio-decoders） | 不明 | `receiver/third_party/opus-decoder/` に同梱（7da57ca で追加） | 手動。SHA-256 `fd73ee0a9c8a5e233c0b88234df14f46003ad89bb4ba27435bc8714db2a6dc62`。版の特定は P0 の残作業 |

ビルドだけに使うもの: typescript 7.0.2、vite 8.3.5（npm、Dependabot）。ビルドは `npm ci` で lockfile どおりに入れる。

## CI だけで使うもの

| 依存物 | 版 | 備考 |
|---|---|---|
| GitHub Actions | actions/checkout v7、setup-node v7、cache v6、upload-artifact v7、download-artifact v8、softprops/action-gh-release v3 | Dependabot。メジャーバージョンのタグで参照しており、コミットでは固定していない |
| Node.js | 20 | `setup-node` で指定 |
| md-to-pdf、github-markdown-css | 未固定 | README の PDF 化に `npm install --no-save` で毎回最新を取得する |

## 追跡の分担

- Dependabot: GitHub Actions と `receiver/` の npm 依存物。
- 手動: cloudflared、FFmpeg、WebSocket++、libopus、Asio、OBS、opus-decoder、md-to-pdf。リリース前と定期確認（計画書の 10 章）で公式の告知を確認する。
