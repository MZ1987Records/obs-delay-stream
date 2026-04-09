#pragma once

#include <obs-module.h>

namespace ods::widgets {

	/**
	 * OBS プロパティパネルにプログレスバーを埋め込む。
	 *
	 * 使い方:
	 * 1. obs_properties_add_flow_progress() でプレースホルダーを追加
	 * 2. schedule_flow_progress_inject() で QProgressBar に差し替え（再構築後に呼ぶ）
	 * 3. update_flow_progress() で再構築なしに値を直接更新
	 * 4. ソース破棄時に flow_progress_unregister_source() を呼ぶ
	 */

	/// プレースホルダーとして OBS_TEXT_INFO プロパティを追加する。bar_text を指定するとバー内にその文字列を表示する（省略時は "%p%"）
	obs_property_t *obs_properties_add_flow_progress(
		obs_properties_t *props,
		const char       *prop_name,
		const char       *row_label,
		int               value,
		const char       *bar_text = nullptr);

	void schedule_flow_progress_inject(obs_source_t *source);   ///< プレースホルダーを実ウィジェットへ差し替える inject を予約する
	void update_flow_progress(obs_source_t *source, int value); ///< 既存のプログレスバー値を直接更新する（プロパティ再構築なし）
	void flow_progress_unregister_source(obs_source_t *source); ///< ソースに紐づいたプログレスバー状態を解放する

} // namespace ods::widgets
