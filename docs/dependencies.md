# 依存物一覧

`docs/security-hardening-plan.md` の P0「現物把握」のうち、リポジトリから確認できる範囲をまとめる。利用者の PC に実際に置かれている exe の版・ハッシュ・署名は含まない（P0 の残作業）。

確認日: 2026-10-24。版を更新したら、この一覧と確認日を併せて更新する。

## プラグインが実行時に取得する実行ファイル

いずれも取得物を検証しておらず、承認版の固定は P1 で行う。

| 依存物 | 取得元 | 現状 | 追跡 |
|---|---|---|---|
| cloudflared | `https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-windows-amd64.exe` | `latest` を取得。既存の exe があれば再利用し、取得後は存在だけを確認する | 手動。Cloudflare の告知とリリースを確認する |
| FFmpeg | `https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip` | `master-latest` の ZIP を取得・展開。既存の exe があれば再利用する | 手動。BtbN の日付付きビルドは保持期間が保証されない |

取得処理: `src/tunnel/tunnel-manager.cpp`（cloudflared）、`src/network/rtsp-e2e-prober.cpp`（FFmpeg）。

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
- 手動: cloudflared、FFmpeg、WebSocket++、libopus、Asio、OBS、opus-decoder、md-to-pdf。リリース前と定期確認（計画書の 9 章）で公式の告知を確認する。
