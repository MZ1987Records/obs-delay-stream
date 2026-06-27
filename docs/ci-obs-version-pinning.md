# CI/ローカルビルドの OBS バージョン固定 設計メモ

GitHub Actions で出力した配布物（`obs-delay-stream-vX.Y.Z.zip`）が、利用者の OBS で
**プラグインとしてロードされない**問題の原因分析と、再発させないための仕組みの設計をまとめる。

> ステータス: **実装済み**（案 C / 下限 32.0.4 / CI 配線・import ガード）。
> 方針: **案 C（FFmpeg 依存の排除）**（§7）／**サポート下限 = OBS 32.0（基準タグ `32.0.4`）**（§8）。
> デグレ確認手順: [`regression-check-ffmpeg-removal.md`](regression-check-ffmpeg-removal.md)。
> 実装反映: `src/network/opus-encoder.*`, `src/network/stream-router.cpp`,
> `CMakeLists.txt`, `OBS_STUDIO_REF`, `.github/workflows/build.yml`, `build.bat`, `third_party/opus`。
>
> 別件: 低レイテンシ再生の安定化（受信側 AudioWorklet 化・TCP_NODELAY・COOP/COEP）は
> [`receiver-audio-pipeline.md`](receiver-audio-pipeline.md) を参照。`stream-router.cpp` には
> この別件の変更（TCP_NODELAY / COOP・COEP ヘッダ）も含まれる。

---

## 1. 事象

- `build.bat` でローカルビルドした DLL は OBS でロードされる。
- GitHub Actions がビルドした同一バージョン（v8.1.2）の DLL がロードされない／フィルター一覧に出ない。
- OBS を 32.2 へ更新しても同症状。

OBS ログ（`%APPDATA%\obs-studio\logs`）に以下が記録される:

```
LoadLibrary failed for '...obs-delay-stream.dll': The specified module could not be found.
Module '...obs-delay-stream.dll' not loaded
Failed to create source 'obs-delay-stream'!
```

`The specified module could not be found` は Windows error 126（**依存 DLL の解決失敗**）。
libobs のバージョン互換チェック（後述）による拒否ではない。

---

## 2. 根本原因：FFmpeg メジャーバージョンの不一致

プラグイン DLL は OBS 同梱の FFmpeg（libavcodec 等）を直接 import している。
import する DLL 名には **メジャー番号が埋め込まれる**（`avcodec-61.dll` 等）ため、
リンク時の FFmpeg 世代とランタイム（利用者の OBS 同梱物）の世代が一致しないとロードできない。

| プラグインが import | OBS 32.2.0-beta2 が同梱 |
|---|---|
| `avcodec-61.dll`（FFmpeg 7.0） | `avcodec-62.dll`（FFmpeg 7.1） |
| `avutil-59.dll` | `avutil-60.dll` |
| `swresample-5.dll` | `swresample-6.dll` |

`avcodec-61.dll` が OBS 32.2 に存在しないため、`LoadLibrary` が error 126 で失敗する。

### なぜローカルは「動いていた」のか
ローカル `third_party/obs-studio/.deps` も同じ `obs-deps-2025-08-23`（avcodec-61 = FFmpeg 7.0）で、
当時の OBS 32.1 も avcodec-61 を同梱していたため**たまたま一致**していた。
OBS 32.2 で FFmpeg が avcodec-62 へ上がった結果、ローカルビルドも同じ理由でロード不可になる
（＝「OBS を 32.2 にしても同症状」は当然の帰結）。

### 補足：libobs API バージョンは主因ではない
観測値: CI ビルド `obs_module_ver = 0x20020000`（32.2.0）／ローカル `0x20010000`（32.1.0）。
libobs のローダ [`libobs/obs-module.c`](../third_party/obs-studio/libobs/obs-module.c) は
patch 以下（下位 16bit）を無視し、**プラグインが libobs より新しい時だけ**拒否する:

```c
/* Reject plugins compiled with a newer libobs. Patch version (lower 16-bit) is ignored. */
uint32_t ver = mod.ver ? mod.ver() & 0xFFFF0000 : 0;
if (ver > LIBOBS_API_VER) { ... return MODULE_INCOMPATIBLE_VER; }
```

利用者は 32.2 なので 32.2.0 のプラグインは拒否されない。今回の主因は FFmpeg DLL 名の不一致。

---

## 3. なぜ CI で古い FFmpeg がリンクされたか

CI ログ（最終ビルド）より:

- `OBS_STUDIO_REF: master` を解決 → `master => 2396e6e9...`（流動的な master HEAD）。
- その master を `obs-deps-2025-08-23-x64`（FFmpeg 7.0 / avcodec-61）でビルド。
- OBS ビルドは **キャッシュ exact hit**（`Windows-obs-build-ref-master-sha-2396e6e9...`）で
  古い `.deps` ごと再利用。

加えて、リリースとして利用者へ渡す対象は**特定の OBS リリース**なのに、
ビルド基準が**流動的な master**であるため、FFmpeg 世代がリリース同梱物とずれ得る構造になっている。

---

## 4. 設計：確実にロードされる（受け入れられる）仕組み

> **案 C 採用による位置づけの変化（§7 参照）**
> FFmpeg import を排除すると、§2 の「FFmpeg 世代不一致」は**構造的に消える**。
> その結果、本節の仕組みは「FFmpeg 世代合わせ」ではなく、
> **libobs 側の互換性（API バージョンと obs.dll シンボル）を、サポート下限 OBS に合わせる**役割に変わる。
> - ① 固定先は「**サポートする最も古い OBS**（下限）」にする（古い OBS で拒否されず、新しい OBS でも `ver <= ランタイム` で受理されるため、下限に合わせると上方互換になる）。
> - ④ ガードは「FFmpeg 世代一致」ではなく「**`libav*` / `libsw*` を一切 import していないこと**」の検査に変わる（より単純・強力）。

本質は **①対象 OBS の固定** と **④ロード互換ガード**。②③ は再発防止。

### ① 対象 OBS を1か所で固定（Single Source of Truth）
- `OBS_STUDIO_REF` を `master` ではなく **OBS リリースタグ**にする。
- その値を **CI（`.github/workflows/build.yml`）とローカル（`build.bat`）で共有**する
  （例: リポジトリ直下の `OBS_STUDIO_REF` テキスト等、両者から参照できる単一定義）。
- バージョン更新時はこの1か所だけ変更する運用。
- 効果: プラグインがリンクする FFmpeg 世代が、対象 OBS リリース同梱物と必ず一致。

**未確定事項**: 固定先タグ（最新安定 `32.2.x` か、手元の `32.2.0-beta2` か）。
FFmpeg メジャーは対象 OBS と連動するため、ここはサポート方針として別途決定する。

### ② CI キャッシュを「世代跨ぎ不可」にする
- 現状の restore-keys（[`build.yml`](../.github/workflows/build.yml) の `Windows-obs-build-` まで広がるフォールバック）が、
  **古い FFmpeg を含む OBS ビルドを拾う**温床。
- フォールバックを「同一 ref ＋同一 SHA」までに限定し、最広の `-obs-build-` を削除。
- ref を変えればキーが変わり `.deps` ごと取り直される。

### ③ build.bat の再同期（ローカルが古いまま固定される問題）
- 現状 `build.bat` は既存 `third_party/obs-studio` を使い回し、ref が変わっても更新しない。
- 「ビルド済み OBS の ref」を記録し、`OBS_STUDIO_REF` と異なる場合は
  `.deps` と `build_x64` を破棄して再 checkout・再ビルドする。

### ④ ロード互換ガード（ビルド時検査）★出荷前に必ず検知
- ビルドした `obs-delay-stream.dll` が import する `avcodec-NN` を、
  使用した obs-deps の `avcodec-NN` と突き合わせ、**不一致ならビルドを失敗**させる。
- CI とローカル双方に入れることで、「出荷後に初めてロード不可が判明」を防ぐ。
- 実装手段の候補: `dumpbin /imports`（VS 付属）または `llvm-objdump -p` で import 名を抽出し比較。

---

## 5. 変更予定ファイル（実装時の対象・未着手）

| ファイル | 変更概要 | 対応する設計項目 |
|---|---|---|
| `src/network/opus-encoder.{hpp,cpp}` | FFmpeg→libopus + libobs リサンプラ へ全面置換（§7） | C |
| `src/network/stream-router.cpp` | OpusEncoder 呼び出し側の型・引数調整（API は極力据え置き） | C |
| `third_party/opus`（新規 vendoring） | libopus を静的リンク用に取得（websocketpp/asio と同方式） | C |
| `CMakeLists.txt` | `libav*`/`libsw*` リンク削除、libopus 静的リンク追加 | C |
| 単一定義（例 `OBS_STUDIO_REF` ファイル） | 対象 OBS タグ（下限）の唯一の真実を新設 | ① |
| `.github/workflows/build.yml` | ref を単一定義から読む／キャッシュキー厳格化／**FFmpeg import 不在**検査 | ①②④ |
| `build.bat` | 単一定義を読む／ref 変更時に `.deps`・`build_x64` 再同期／libopus 取得・同検査 | ①③④ |
| 本ドキュメント | 設計の根拠と手順を保守 | 全体 |

---

## 6. 未決事項

1. ~~固定先 OBS バージョン（下限）~~ → **OBS 32.0 に確定（§8）**。
   ただし「32.0 時点の libobs API / obs.dll シンボルしか使わない」ことは
   実装時に担保が必要（`32.0.4` ヘッダ・lib でビルドが通ることが自然なガードになる）。
2. リサンプラの実装手段：libobs `audio_resampler`（obs.dll エクスポート、新規依存なし）を使うか、
   自前 SRC を vendoring するか（§7.3）。
3. libopus の取得・ビルド方法（vendoring + CMake 静的ビルド or 既製 static lib）。
4. ④ 検査の実装手段（`dumpbin /imports` or `llvm-objdump -p`、CI ランナー前提）。
5. 実装範囲（C 本体を先行し、②③ の再発防止は後追いにするか）。

---

## 7. 採用案 C の詳細設計：FFmpeg 依存の排除

### 7.1 現状の FFmpeg 用途（[`src/network/opus-encoder.cpp`](../src/network/opus-encoder.cpp)）

| 機能 | 使用ライブラリ | 主な API |
|---|---|---|
| Opus エンコード | libavcodec | `avcodec_find_encoder_by_name("libopus")`, `avcodec_open2`, `avcodec_send_frame`, `avcodec_receive_packet` |
| リサンプル / フォーマット変換 | libswresample | `swr_alloc_set_opts2`, `swr_convert`, `swr_get_delay` |
| フレーム境界調整 FIFO | libavutil | `av_audio_fifo_*` |
| フレーム/パケット/レイアウト | libavutil | `av_frame_*`, `av_packet_*`, `av_channel_layout_*`, `av_samples_alloc_*` |

入出力の性質（確認済み）:
- 入力 `feed_fifo(const float *data, size_t frames)` は **インターリーブ float**（単一プレーン）。
- 出力は `[magic, sample_rate, channels, frames]` の 16B ヘッダ + Opus パケット本体。
- 対象サンプルレートは `is_valid_opus_sample_rate()` 準拠（8/12/16/24/48k）。非対応時 48k。
- フレーム単位送出（端数は破棄）、`pts` は連続維持、ディレイ変更時に `drain` でフラッシュ。

### 7.2 置換マッピング

| 現行（FFmpeg） | 置換後（FFmpeg 非依存） |
|---|---|
| `avcodec` libopus エンコーダ | **libopus 直叩き**（`opus_encoder_create` / `opus_encode_float` / `opus_encoder_ctl` / `opus_encoder_destroy`）を**静的リンク** |
| `ctx->bit_rate` / `compression_level` | `opus_encoder_ctl(OPUS_SET_BITRATE / OPUS_SET_COMPLEXITY / OPUS_SET_VBR)` |
| `ctx->frame_size`（20ms=960@48k） | `frame_size = output_sample_rate / 50`（20ms）を明示採用 |
| `swr_*` リサンプル | **libobs `audio_resampler`**（`media-io/audio-resampler.h`、obs.dll エクスポート）。dst を `AUDIO_FORMAT_FLOAT`(インターリーブ) にして取り出す |
| `av_audio_fifo_*` | **`std::vector<float>` ベースのリングバッファ**（インターリーブ、`frames*channels` 単位） |
| `AVFrame`/`AVPacket` | 不要（libopus は float バッファ→バイト列で直接エンコード） |
| `av_channel_layout_*` | `channels`（int）のみで完結 |

エンコード本体の流れ（置換後）:
1. `feed_fifo`：入力 float をレート不一致時のみ `audio_resampler` で 48k 等へ変換 → リングバッファへ追記。
2. `drain`：リングバッファから `frame_size*channels` を取り出し `opus_encode_float` → 16B ヘッダ付与で `out` へ。端数は破棄、`pts` 継続。

### 7.3 リサンプラの選択

- **第1候補: libobs `audio_resampler`**。obs.dll が公開しており**新規依存ゼロ**。内部実装は
  swresample だが obs.dll 内に閉じるため、プラグインの import には現れない（＝世代非依存）。
- 代替: 自前 SRC を vendoring。依存は減るが実装・品質責任が増える。
- 注: OBS 出力が 48k のときはリサンプル不要。44.1k 等のときのみ 48k へ変換が必要。

### 7.4 libopus の組み込み

- `third_party/opus` として vendoring（既存の websocketpp / asio と同じ流儀）。
- CMake で**静的ライブラリとしてビルドしプラグインへ静的リンク**（追加ランタイム DLL なし）。
- libopus は小さく ABI が安定。`opus.dll` を OBS が同梱しない前提のため、動的リンクは選ばない。

### 7.5 期待される結果（import 表）

置換後のプラグイン import から `avcodec-*` / `avutil-*` / `swresample-*` が**消える**。
残るのは `obs.dll` ＋ `Qt6*` ＋ システム（MSVCP140 等）のみ。
→ FFmpeg 世代に依存せず、libobs API が合う**任意の OBS バージョンでロード可能**。

### 7.6 リスク / 確認事項

- **音質・互換の同等性**: avcodec 経由の libopus と直叩き libopus は同一エンジンだが、
  既定パラメータ（VBR/DTX/フレーム長/complexity）の差異を合わせ込む必要あり。
- **リサンプラ等価性**: `audio_resampler` の品質・遅延が swresample と差異ないか要確認。
- **フレーム長**: 20ms 固定にすると、現行の `ctx->frame_size` 依存箇所（ヘッダの `pkt_frames`）を
  `frame_size` で一貫させる。
- **チャンネル/レイアウト**: モノ/ステレオ前提。多チャンネル時の挙動を現行と揃える。
- **CI ビルド**: libopus 静的ビルドを CI/ローカル双方で再現可能にする（キャッシュ対象に含める）。

---

## 8. サポート下限の決定：OBS 32.0（基準タグ `32.0.4`）

### 8.1 根拠とした OBS リリース履歴（基準日 2026-06-26）

| 時期 | OBS 安定版 | libobs API | obs-deps | FFmpeg / avcodec |
|---|---|---|---|---|
| 約9か月前（2025-09） | 32.0.0 | 32.0 | 2025-08-23 | 7.0 / avcodec-61 |
| 約6か月前（2025-12） | **32.0.4** | 32.0 | 2025-08-23 | 7.0 / avcodec-61 |
| 約3.5か月前（2026-03） | 32.1.0 | 32.1 | 2025-08-23 | 7.0 / avcodec-61 |
| 約2か月前（2026-04） | 32.1.2 | 32.1 | 2025-08-23 | 7.0 / avcodec-61 |
| 現在（beta） | 32.2.0-beta2 | 32.2 | 2026-06-02 | 7.1 / avcodec-62 |

判明した重要事実: **FFmpeg が 7.0→7.1（avcodec-61→62）に上がったのは 32.2 のみ**。
32.0 と 32.1 はいずれも avcodec-61。現行の avcodec-61 ビルドが「32.1 以前では動き、32.2 で壊れた」
のはこのためで、§1〜§2 の症状と整合する。

### 8.2 決定

- **サポート下限 = OBS 32.0**、**ビルド基準タグ = `32.0.4`**（32.0 系の最新パッチ）。
- 理由:
  - 32.0.0 は約9か月前であり、「半年前」を十分カバー。現行 32.2 から2マイナー前で保守負担も小さい。
  - 案 C 後は FFmpeg 世代が無関係になるため、下限を下げるコストは
    「32.0 時点の libobs API のみ使用」の担保だけ。
  - `32.0.4` 基準なら `obs_module_ver` は 32.0 として焼かれ、patch 以下は無視されるため
    **32.0.x〜32.2 以降すべてで受理**（上方互換）。

### 8.3 留意点

- プラグインが 32.1 / 32.2 で追加された libobs API を使っていないこと。
  `32.0.4` のヘッダ・lib でビルドが通ることが事実上のガードになる。
- §4 ① の `OBS_STUDIO_REF` 単一定義の値は **`32.0.4`** とする。
- 将来 32.2 系の安定版が普及して下限を上げる場合も、この値を1か所変えるだけで済む（§4①）。
