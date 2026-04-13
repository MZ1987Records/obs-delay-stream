---
name: mvvm-architecture
description: Enforce this project's MVVM architecture rules when the task involves adding or changing settings parameters, delay calculations (DelayState, calc_all_delays), SettingsRepo accessors, DelayViewModel fields, UI properties tabs (properties-*.cpp, get_properties), modified callbacks (obs_property_set_modified_callback), custom widget injection (schedule_*_inject), or channel operations (copy_channel, clear_channel, swap_channels).
allowed-tools: Read, Edit, Write, Grep, Glob, Bash
---

# MVVM アーキテクチャ規約の適用

パラメータ追加・ディレイ計算変更・UI 改修を行う際に、プロジェクト固有の MVVM アーキテクチャ規約を適用する。

背景情報（全体構造、データフロー、スレッドモデル）は [`docs/architecture-mvvm.md`](docs/architecture-mvvm.md) を参照すること。このスキルはその中の手続き的ルールを強制する。

## 層の責務

| 層 | ディレクトリ | やること | やらないこと |
|---|---|---|---|
| View | `src/ui/` | ViewModel の const 参照を読んで UI を構築。ボタン CB は Model/SettingsRepo を更新 | `DelayStreamData` のフィールドを直接変更しない |
| ViewModel | `src/viewmodel/` | `build()` で Model + obs_data から毎回構築。スタックローカルで消費 | メンバ変数やグローバルに保持しない |
| Model | `src/model/` | `DelayState` でディレイ計算、`SettingsRepo` で obs_data アクセス | UI 層を直接知らない |
| Infra | `src/plugin/` | `DelayStreamData`, `SettingsApplier`, イベント CB | — |

## 禁止事項

以下に該当するコードを書こうとした場合、必ず停止して正しいパターンに修正する。

1. **`properties-*.cpp` から `obs_data_get_*/set_*` を直接呼ぶ** → `SettingsRepo` 経由にする
2. **`calc_all_delays()` 以外の場所でディレイを計算する** → `DelayState::calc_all_delays()` に集約する
3. **`get_properties()` 内で `DelayStreamData` のフィールドを変更する** → UI 構築は読み取り専用
4. **ViewModel をメンバ変数やグローバルに保持する** → スタックローカルで毎回構築する
5. **`obs_property_t*` をコールバック間でキャッシュする** → OBS は毎回再構築する設計
6. **計算済み表示データを `obs_data_t` に永続保存する** → 入力値のみ保存し都度計算する
7. **カスタムウィジェットがあるページで `return true` 後に inject を再スケジュールしない** → §CB ルール参照
8. **カスタムウィジェットのデストラクタで binding_id マップエントリを削除する** → `RefreshProperties` がウィジェットを再構築しても `obs_properties_t` は生存しているため、再 inject で同じ binding_id が必要になる。古いエントリは登録関数（`obs_properties_add_*_row`）で新しい binding_id を作る際にプレフィックス一致で掃除する

## modified callback の `return true` ルール {#cb-rule}

### OBS の RefreshProperties メカニズム

OBS の modified callback が `true` を返すと `RefreshProperties()` が走る。重要なのは **`RefreshProperties()` は既存の `obs_properties_t` を再トラバースするだけで `get_properties()` を再呼出ししない** こと。つまり `schedule_widget_injects_for_tab()` は呼ばれず、`OBS_TEXT_INFO` プレースホルダー経由で注入されたカスタムウィジェット（ColorButtonRow, PulldownRow, StepperRow, HelpCallout 等）は破壊されたまま復元されない。

> **参照**: `third_party/obs-studio/shared/properties-view/properties-view.cpp` の `RefreshProperties()` — `obs_properties_first(properties.get())` で既存オブジェクトを走査。

### inject の実行タイミング

このプロジェクトの OBS の `ui_task_handler`（`OBSApp.cpp`）は `Qt::AutoConnection` を使用するため、UI スレッドから呼ばれた `obs_queue_task(OBS_TASK_UI, ..., false)` は **DirectConnection（同期実行）** になる。

コールバック本体で `schedule_*_inject` を呼ぶと:

1. inject 関数が **同期的に即座に** 実行される
2. 旧プレースホルダーは既に前回の inject で置換済み → ラベルが見つからない → **リトライ開始**（`QTimer::singleShot`）
3. コールバックが `true` を返す → OBS が `RefreshProperties()` をキューイング
4. `RefreshProperties()` がウィジェットを再構築 → 新しいプレースホルダーが生まれる
5. リトライが新しいプレースホルダーを検出して置換 → **復元完了**

**ルール**: `return true` する modified callback は、そのタブに必要な **すべての** inject を `props_ui_with_preserved_scroll` 内でスケジュールすること。1 種類でも漏れると、その種別だけ未置換のプレースホルダーがユーザーに見える。

```cpp
// OK: return true + 当該タブの全 inject を再スケジュール
// タブ 1 (WS配信) の場合
bool cb_xxx_changed(void *priv, obs_properties_t *props, obs_property_t *, obs_data_t *settings) {
    auto *d = static_cast<DelayStreamData *>(priv);
    // ... visibility/enabled 変更 ...
    props_ui_with_preserved_scroll([d]() {
        if (!d || !d->context) return;
        schedule_color_button_row_inject(d->context);  // 常に必要
        schedule_pulldown_row_inject(d->context);       // タブ 1 固有
        schedule_stepper_inject(d->context);            // タブ 1 固有
        schedule_help_callout_inject(d->context);       // タブ 1 固有
    });
    return true;
}

// NG: inject 漏れのある return true
bool cb_xxx_changed(...) {
    // ...
    props_ui_with_preserved_scroll([d]() {
        schedule_color_button_row_inject(d->context);
        schedule_stepper_inject(d->context);
        // schedule_help_callout_inject が漏れている → ヘルプ表示が壊れる
    });
    return true;
}

// NG: inject なしの return true
bool cb_xxx_changed(...) {
    obs_property_set_visible(p, show);
    return true;  // 全カスタムウィジェットが消える
}
```

### ボタンコールバックとの違い

ColorButtonRow / TextButton 等のカスタムウィジェット経由のボタンクリックは、コールバックが `true` を返すと `obs_source_update_properties()` を呼ぶ。これは **`get_properties()` を再呼出し** するフルリロードのため、`schedule_widget_injects_for_tab()` が自動的に走る。ボタンコールバック本体で inject を手動スケジュールする必要はない。

### なぜ `return false` + `request_props_refresh` ではダメか

OBS はダイアログ構築時にも modified callback を呼ぶ。その時点で `get_props_depth` は 0 のため `request_props_refresh` がガードを通過し、`get_properties` → 構築 → callback → refresh → … の無限ループになる。

### タブ別の必要 inject 一覧

`schedule_widget_injects_for_tab()` (plugin-main.cpp) を正とする。新しいカスタムウィジェットを追加した場合、**この関数**と**以下の表**と**同タブの全 modified callback 本体**の 3 箇所を更新する。

| タブ | 常に必要 | タブ固有 |
|---|---|---|
| 0 (出演者名) | color_button_row | text_button |
| 1 (WS配信) | color_button_row | pulldown_row, stepper, help_callout |
| 2 (URL共有) | color_button_row | text_button, path_mode_row, url_table, help_callout |
| 3 (WS計測) | color_button_row | flow_progress, flow_table |
| 4 (RTSP計測) | color_button_row | flow_progress, path_mode_row, mode_text_row |
| 5 (遅延) | color_button_row | stepper, delay_diagram, delay_table |

## パラメータ追加チ��ックリスト

新しい設定パラメータを追加したら以下を順に確認する。各ステップを完了してから次に進む。

### ディレイ計算に影響するパラメータ

- [ ] `DelayState` にフィールド追加 (`src/model/delay-state.hpp`)、`calc_all_delays()` を更新
- [ ] `SettingsRepo` にアクセサ追加 (`src/model/settings-repo.hpp`)。キーが新規なら `plugin-settings.hpp` にも追加
- [ ] `SettingsApplier::apply_delay_settings()` で obs_data → `d->delay.xxx` への転写を追加
- [ ] 必要なら `DelayViewModel` に表示フィールドを追加し `build()` で値を設定
- [ ] UI 構築関数を更新（ViewModel から読み取って表示）
- [ ] ロケールキー追加 (`data/locale/`)

### ディレイ計算に影響しないパラメータ

- [ ] `SettingsRepo` にアクセサ追加（obs_data 経由の場合）
- [ ] `SettingsApplier` で転写
- [ ] UI 構築関数を更新

### チャンネル別パラメータ（上記に加えて）

- [ ] `SettingsRepo` の `copy_channel()` / `clear_channel()` / `swap_channels()` を更新
- [ ] `plugin-settings.hpp` に `make_sub_*_key(int ch)` を追加

## UI 改修チェックリスト

### 遅延タブ (tab 5)

- [ ] 表示データは `DelayViewModel` 経由で取得。`DelayStreamData` を直接参照しない
- [ ] 新しい表示項目は `DelayViewModel::ChDisplay` またはルートにフィールド追加 → `build()` で計算

### チャンネル管理タブ (tab 0)

- [ ] `obs_data` 操作は `SettingsRepo` 経由
- [ ] 新しいチャンネル別フィールドを追加したら `SettingsRepo` の一括操作を更新

### その他のタブ

- ViewModel 未導入タブ（WS配信、トンネル等）は `DelayStreamData` を直接参照して良い

### 新しいカスタムウィジェット種別を追加する場合

- [ ] binding_id は `prop_name#seq` 形式で一意に生成する（既存の `make_*_binding_id` 関数を参照）
- [ ] デストラクタで binding_id マップエントリを **削除しない**（禁止事項 8 参照）
- [ ] 登録関数（`obs_properties_add_*_row`）で新しい binding_id 登録時に、同じ prop_name プレフィックスの旧エントリを掃除する
- [ ] `schedule_widget_injects_for_tab()` にタブ別の inject 呼び出しを追加する
- [ ] **同じタブにある `return true` する全 modified callback** の `props_ui_with_preserved_scroll` 本体にも inject 呼び出しを追加する
- [ ] 上記のタブ別 inject 一覧表を更新する
- 複雑度が上がったら `src/viewmodel/` に ViewModel を導入する

## レビュー手順

タスク完了前に以下を確認する。

1. 追加・変更したコードが上記の禁止事項に該当しないこと
2. パラメータ追加の場合、チェックリストの全項目が完了していること
3. modified callback を追加・変更した場合、`return true` ルールに従っていること
4. チャンネル別パラメータの場合、`SettingsRepo` の一括操作が更新されていること
5. **inject クロスチェック**: カスタムウィジェットの追加・変更を行った場合、以下の 3 箇所が同期していること
   - `schedule_widget_injects_for_tab()` (plugin-main.cpp)
   - 同タブの全 modified callback の `props_ui_with_preserved_scroll` 本体
   - §CB ルールのタブ別 inject 一覧表（このスキル内）
