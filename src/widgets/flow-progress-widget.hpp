#pragma once

#include <obs-module.h>

// OBSプロパティパネルにプログレスバーを埋め込む。
//
// 使い方:
//   1. obs_properties_add_flow_progress() でプレースホルダーを追加
//   2. schedule_flow_progress_inject() でQProgressBarに差し替え（再構築後に呼ぶ）
//   3. update_flow_progress() でプロパティ再構築なしに値を直接更新
//   4. ソース破棄時に flow_progress_unregister_source() を呼ぶ

obs_property_t *obs_properties_add_flow_progress(
	obs_properties_t *props,
	const char       *prop_name,
	const char       *row_label,
	int               value);

void schedule_flow_progress_inject(obs_source_t *source);
void update_flow_progress(obs_source_t *source, int value);
void flow_progress_unregister_source(obs_source_t *source);
