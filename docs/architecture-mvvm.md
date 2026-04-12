# MVVM アーキテクチャガイド

本プロジェクトでは、OBS プラグイン API の制約下で MVVM 風の 3 層アーキテクチャを採用している。
パラメータ追加・UI 改修を行う際は、このドキュメントに従うこと。

---

## 1. 全体構造

```
┌──────────────────────────────────────────────┐
│  View (OBS properties + Qt widgets)           │
│  get_properties() で ViewModel を読むだけ      │
│  ボタン CB は Model / SettingsRepo を更新       │
└───────────────┬──────────────────────────────┘
                │ reads (const 参照)
┌───────────────┴──────────────────────────────┐
│  ViewModel (読み取り専用スナップショット)        │
│  例: DelayViewModel                           │
│  build() で Model + obs_data から毎回構築      │
│  計算済み表示データを保持                       │
└───────────────┬──────────────────────────────┘
                │ reads
┌───────────────┴──────────────────────────────┐
│  Model (計算ロジック + 永続化ファサード)        │
│  DelayState — ディレイ計算の入力値と計算関数    │
│  SettingsRepo — obs_data_t の型安全ラッパー    │
└──────────────────────────────────────────────┘
```

### ディレクトリ構成

| ディレクトリ | 層 | 内容 |
|---|---|---|
| `src/model/` | Model | `DelayState`, `SettingsRepo` |
| `src/viewmodel/` | ViewModel | `DelayViewModel` |
| `src/ui/` | View | `properties-*.cpp` (OBS プロパティ構築) |
| `src/plugin/` | Infrastructure | `DelayStreamData`, `SettingsApplier`, イベント CB |

---

## 2. 主要コンポーネント

### 2.1 DelayState / DelaySnapshot (`src/model/delay-state.hpp`)

**ディレイ計算の唯一の真実 (single source of truth)**。

- `DelayState` — 計算に必要な入力値（計測結果、オフセット、アバター遅延など）
- `DelaySnapshot` — `calc_all_delays()` が返す不変の計算結果

```cpp
// 入力値
struct DelayState {
    int  measured_rtsp_e2e_ms;
    int  avatar_latency_ms;
    int  playback_buffer_ms;
    int  sub_ch_count;
    std::array<ChDelay, MAX_SUB_CH> channels; // measured_ms, ws_measured, offset_ms
    DelaySnapshot calc_all_delays() const;
};

// 計算結果（不変）
struct DelaySnapshot {
    std::array<ChDelay, MAX_SUB_CH> channels; // raw_ms, total_ms, has_measurement, warn
    int neg_max_ms;
    int master_delay_ms;
    int active_count;
};
```

**ルール**:
- ディレイ値の計算は **必ず** `calc_all_delays()` を経由する。個別計算を散在させない。
- `DelayStreamData` は `DelayState delay;` メンバを持ち、ディレイ関連フィールドはすべてここに集約する。

### 2.2 SettingsRepo (`src/model/settings-repo.hpp`)

**`obs_data_t` への型安全アクセスファサード**。

```cpp
class SettingsRepo {
    obs_data_t *s_;
public:
    // チャンネル別
    int  ch_measured_ms(int ch) const;
    void set_ch_measured_ms(int ch, int v);
    // ... ch_ws_measured, ch_offset_ms, ch_memo, ch_code

    // 一括操作
    void shift_channels_down(int from_ch);
    void swap_channels(int ch_a, int ch_b);
    void clear_channel(int ch);

    // グローバル
    int  sub_ch_count() const;
    void set_sub_ch_count(int v);
    int  memo_auto_counter() const;
    void set_memo_auto_counter(int v);
    void set_measured_rtsp_e2e_ms(int v);
    void set_rtsp_e2e_measured(bool v);
};
```

**ルール**:
- UI コールバック (`properties-*.cpp`) から `obs_data_get_*/set_*` を直接呼ばない。必ず `SettingsRepo` 経由。
- 新しい obs_data キーを追加する場合、SettingsRepo にアクセサを追加してから使う。
- キー名文字列は `plugin-settings.hpp` の `make_sub_*_key()` または SettingsRepo 内に定義する。外部に散在させない。

### 2.3 DelayViewModel (`src/viewmodel/delay-viewmodel.hpp`)

**遅延タブ UI 用の読み取り専用スナップショット**。

```cpp
struct DelayViewModel {
    struct ChDisplay { name, measured_ms, offset_ms, raw_delay_ms, neg_max_ms, total_ms, warn };
    DelaySnapshot          snapshot;
    std::vector<ChDisplay> channels;
    int                    selected_ch;

    static DelayViewModel build(const DelayState &delay, obs_data_t *settings);
};
```

**ルール**:
- `get_properties()` の冒頭で `build()` し、UI 構築関数に `const` 参照で渡す。
- ViewModel は **スタックローカル** で構築・消費する。メンバ変数に保持しない。
- UI 構築関数は ViewModel のフィールドだけを読み、`DelayStreamData` を直接参照しない。

---

## 3. データフロー

### 3.1 設定変更 → ディレイ反映

```
obs_data_t 変更 (OBS update コールバック)
  → SettingsApplier::apply_delay_settings()
    → d->delay.xxx へ値を転写
    → recalc_all_delays(d)
      → d->delay.calc_all_delays()  → DelaySnapshot
      → 各 DelayBuffer へ total_ms を適用
```

### 3.2 計測完了 → 設定書き戻し → ディレイ反映

```
計測コールバック (flow.on_ws_measured 等)
  → queue_ui_safe() で UI スレッドへディスパッチ
    → d->delay.channels[ch].measured_ms = 計測値
    → save_measurement_and_recalc(d)
      → SettingsRepo で obs_data に書き戻し
      → recalc_all_delays(d)
    → request_props_refresh_for_tabs()
```

### 3.3 UI 再描画

```
get_properties(d)
  → アクティブタブに応じて分岐
  → case 5 (遅延タブ):
      obs_data_t *s = obs_source_get_settings(d->context);
      DelayViewModel vm = DelayViewModel::build(d->delay, s);
      obs_data_release(s);
      add_delay_summary_group(props, vm);  // const 参照のみ
```

---

## 4. パラメータ追加チェックリスト

新しい設定パラメータを追加する場合、以下の手順に従う。

### 4.1 ディレイ計算に影響するパラメータの場合

1. **DelayState にフィールド追加** (`src/model/delay-state.hpp`)
   - チャンネル別なら `DelayState::ChDelay` に、グローバルなら `DelayState` 直下に追加
   - `calc_all_delays()` の計算式を更新

2. **SettingsRepo にアクセサ追加** (`src/model/settings-repo.hpp`)
   - キーが新規なら `plugin-settings.hpp` にキー定数/生成関数を追加

3. **SettingsApplier で転写** (`src/plugin/plugin-settings.cpp`)
   - `apply_delay_settings()` で obs_data → `d->delay.xxx` への転写を追加

4. **DelayViewModel に表示フィールド追加** (必要な場合のみ, `src/viewmodel/delay-viewmodel.hpp`)
   - `ChDisplay` や ViewModel 直下にフィールドを追加
   - `build()` で値を設定

5. **UI 構築関数を更新** (`src/ui/properties-delay.cpp`)
   - ViewModel から読み取って表示

6. **ロケールキー追加** (`data/locale/`)

### 4.2 ディレイ計算に影響しないパラメータの場合

1. **SettingsRepo にアクセサ追加** (obs_data 経由のパラメータの場合)

2. **SettingsApplier で転写** (`src/plugin/plugin-settings.cpp`)

3. **UI 構築関数を更新** (`src/ui/properties-*.cpp`)
   - 該当タブの ViewModel がある場合はそこにフィールドを追加
   - ない場合は `DelayStreamData` から直接読み取って良い（将来 ViewModel を導入する余地を残す）

### 4.3 チャンネル別パラメータの場合

上記に加えて:

1. **SettingsRepo の一括操作を更新**
   - `copy_channel()` / `clear_channel()` / `swap_channels()` に新フィールドを追加
   - これを忘れるとチャンネル削除・並替えで値が失われる

2. **キー生成関数を追加** (`plugin-settings.hpp`)
   - `make_sub_*_key(int ch)` を追加

---

## 5. UI 改修チェックリスト

### 5.1 遅延タブ (tab 5) の改修

1. 表示データは **必ず `DelayViewModel` 経由**で取得する
2. `DelayStreamData` を直接参照しない
3. 新しい表示項目は `DelayViewModel::ChDisplay` またはルート構造体にフィールドを追加し、`build()` で計算する

### 5.2 チャンネル管理タブの改修

1. `obs_data` 操作は **`SettingsRepo` 経由**
2. チャンネル追加/削除/並替えのロジックは `properties-channels.cpp` のコールバック内で完結させる
3. 新しいチャンネル別フィールドを追加したら、`SettingsRepo::copy_channel()` / `clear_channel()` / `swap_channels()` を更新する

### 5.3 その他のタブの改修

- 現時点では ViewModel 未導入のタブ（ネットワーク、トンネル等）は `DelayStreamData` を直接参照して良い
- 将来そのタブの複雑度が上がった場合、同様のパターンで ViewModel を導入する

---

## 6. やってはいけないこと

| 禁止事項 | 理由 |
|---|---|
| `properties-*.cpp` から `obs_data_get_*/set_*` を直接呼ぶ | SettingsRepo に集約済み。キー名の散在を防ぐ |
| ディレイ計算を `calc_all_delays()` 以外の場所で行う | 計算ロジックの重複を防ぐ |
| `get_properties()` 内で `DelayStreamData` のフィールドを直接変更する | UI 構築は読み取り専用であるべき |
| ViewModel をメンバ変数やグローバルに保持する | スタックローカルで毎回構築して鮮度を保証する |
| `obs_property_t*` をコールバック間でキャッシュする | OBS API はプロパティを毎回再構築する設計 |
| 計算済みの表示データを `obs_data_t` に永続保存する | 入力値のみ保存し、表示データは都度計算する |

---

## 7. スレッド安全性

| データ | 書込みスレッド | 読取りスレッド | 保護機構 |
|---|---|---|---|
| `DelayBuffer::delay_samples_` | UI (`set_delay_ms`) | Audio (`process`) | atomic (既存) |
| `DelayState` フィールド | UI (`SettingsApplier` / CB) | UI (`ViewModel::build`) | 単一スレッド |
| ViewModel | UI で構築 | UI で消費 | スタックローカル |
| `MeasureState` 内部 | Worker | UI | per-instance mutex |
| `stream_id`, `host_ip` | UI | Audio | `stream_id_mtx` |

新しいフィールドを追加する際は、このスレッドモデルに合わせて適切な保護を選択する。

---

## 8. 将来の拡張方針

現在 ViewModel が導入されているのは遅延タブ (tab 5) のみ。
他のタブ（ネットワーク、トンネル等）の複雑度が上がった場合は、同じパターンで ViewModel を導入する:

1. `src/viewmodel/` に `XxxViewModel` を作成
2. `build()` で必要なデータを事前計算
3. UI 構築関数の引数を `const XxxViewModel &` に変更
4. `get_properties()` 内でアクティブタブのときだけ構築
