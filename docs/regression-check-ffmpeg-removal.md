# デグレ確認手順：FFmpeg 依存排除（案 C）

Opus エンコードを FFmpeg(libav*) から **libopus 直叩き + libobs `audio_resampler`** へ置換した変更
（設計: [`ci-obs-version-pinning.md`](ci-obs-version-pinning.md) §7）について、
回帰（デグレ）が無いかを確認する手順。**A→B→C の順**で実施する。

関連変更:
- [`src/network/opus-encoder.hpp`](../src/network/opus-encoder.hpp) / [`.cpp`](../src/network/opus-encoder.cpp) … libopus 実装へ全面置換
- [`src/network/stream-router.cpp`](../src/network/stream-router.cpp) … `feed()` / `flush()` / `ready()` 呼び出しへ
- [`CMakeLists.txt`](../CMakeLists.txt) … `avcodec/avutil/swresample` リンク削除、`third_party/opus` 静的リンク
- `third_party/opus`（libopus v1.5.2 を vendoring、静的リンク）

> 低レイテンシ再生の安定化（受信側 AudioWorklet 化）の確認は別ドキュメント
> [`receiver-audio-pipeline.md`](receiver-audio-pipeline.md) §5 を参照。

---

## A. ビルドレベル確認（必須・自動化可能）

### A-1. ローカルビルドが通る
```
build.bat
```
- `opus: OK`（クローン）と `Build OK: ...obs-delay-stream.dll` が出ること。

### A-2. FFmpeg を import していないこと ★案 C の核心
```bash
OBJ="/c/Users/ken/scoop/apps/llvm/current/bin/llvm-objdump.exe"
DLL="build/RelWithDebInfo/obs-delay-stream.dll"
"$OBJ" -p "$DLL" | grep -iE "DLL Name" | sort -u
```
- 期待: `avcodec-*` / `avutil-*` / `swresample-*` / `swscale-*` / `avformat-*` が **一つも無い**。
- 残るのは `obs.dll`, `Qt6*`, `MSVCP140/VCRUNTIME140*`, `api-ms-win-crt-*`, `ws2_32`/`wininet`/`iphlpapi`/`mswsock`/`urlmon` 等のみ。
- CI では「Guard against FFmpeg imports」ステップが同じ検査を行い、検出時はビルド失敗。

### A-3. opus が静的リンクされている（余計な DLL 依存が無い）
- A-2 の import 一覧に **`opus.dll` が無い**こと（静的リンクのため）。

### A-4. libobs API バージョン（任意）
```bash
"$OBJ" -d "$DLL" | grep -A1 "<obs_module_ver>"
```
- ローカルは third_party の OBS に依存（例 32.1）。**リリース（CI）は下限 `32.0.4` でビルドされ 32.0 として焼かれる**点を確認。

---

## B. ロードレベル確認（必須・手動）

プラグインが**新旧 OBS の双方で実際にロードされる**ことを確認する。FFmpeg 排除の主目的。

### B-1. 最新 OBS（32.2 系）でロードされる
1. `build.bat` でビルド・インストール（または zip を展開して配置）。
2. OBS を起動 → 音声ソースのフィルターに **obs-delay-stream が表示される**。
3. OBS ログ（`%APPDATA%\obs-studio\logs`）に
   `LoadLibrary failed` / `not loaded` / `compiled with newer libobs` が **出ていない**。

### B-2. 数世代前の OBS（32.0 / 32.1）でロードされる
- 可能なら 32.0.x または 32.1.x を別途用意し、同じ DLL がロードされることを確認。
- ここが旧実装（avcodec-62 依存）からの最大の改善点。

> 確認観点: 「`The specified module could not be found`（error 126）」が**再発していない**こと。

---

## C. 機能レベル確認（必須・手動）：Opus 経路の同等性

旧 FFmpeg 実装と挙動が変わっていないかを、実際の配信で確認する。
受信は receiver（ブラウザ）で再生して耳と目で判定する。

| # | 項目 | 手順 | 期待 |
|---|---|---|---|
| C-1 | Opus 基本再生 | コーデック=Opus で配信し receiver で受信 | 音が正常に出る・途切れない・ノイズが無い |
| C-2 | リサンプル無し経路 | OBS サンプルレート **48kHz**、Opus 出力 48kHz | 正常再生（`audio_resampler` 不生成の経路） |
| C-3 | リサンプル経路 | OBS サンプルレート **44.1kHz**、Opus 出力 48kHz | 正常再生（`audio_resampler` 経路）。ピッチずれ・途切れが無い |
| C-4 | チャンネル | **モノ / ステレオ** 両方 | 左右・定位が正しい |
| C-5 | ビットレート変更 | receiver/設定から bitrate を変更 | 反映され、破綻なく継続 |
| C-6 | 目標サンプルレート切替 | receiver から `sample_rate` 指定を変更 | 切替後も正常再生（init 再実行） |
| C-7 | ディレイ変更フラッシュ | 配信中にチャンネルディレイを変更 | `flush` 後に音声が継続、無音固着や連続ノイズが無い |
| C-8 | PCM フォールバック | コーデック=PCM、または force_pcm 接続 | 従来通り PCM で再生（Opus 経路を通らない） |
| C-9 | 長時間安定性 | 10〜30 分連続配信 | メモリ増加・遅延増大・クラッシュが無い |

### 比較（推奨）
- 同一音源・同一設定で **旧 FFmpeg 版（v8.1.2）と新版**を聴き比べ、音質・体感遅延に大差が無いこと。
- 既定パラメータ差の確認ポイント: VBR（新版は `OPUS_SET_VBR(1)`）、complexity=10、フレーム長=20ms。
  → 旧 avcodec 経由 libopus と挙動が大きく違う場合はここを合わせ込む。

---

## D. 不合格時の主な切り分け

| 症状 | 疑い | 対応 |
|---|---|---|
| A-2 で FFmpeg import が残る | どこかで libav* を使用/リンク | `grep -rnE "libav|av_|swr_" src/` と CMake の link を確認 |
| B でロード不可（error 126） | obs.dll 以外の未解決依存 | import 一覧を再確認。opus が動的化していないか |
| B でロード不可（newer libobs） | CI が下限超のOBSでビルド | `OBS_STUDIO_REF` と CI のキャッシュキーを確認 |
| C-3 で音が乱れる | リサンプラ設定 | `resample_info`（format=`AUDIO_FORMAT_FLOAT`/speakers/rate）を確認 |
| C-7 で無音固着 | flush 後の状態 | `flush()` の端数破棄・`OPUS_RESET_STATE` を確認 |
