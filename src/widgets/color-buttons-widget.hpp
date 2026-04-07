#pragma once

#include <cstddef>
#include <obs-module.h>

namespace ods::widgets {

	/**
	 * 色付きボタン行を生成するための 1 ボタン定義。
	 */
	struct ObsColorButtonSpec {
		const char            *action_prop_name = nullptr; ///< クリック実体プロパティ名
		const char            *button_label     = nullptr; ///< ボタン表示ラベル
		obs_property_clicked_t clicked          = nullptr; ///< クリックコールバック
		void                  *clicked_priv     = nullptr; ///< コールバック引数
		bool                   enabled          = true;    ///< 初期有効状態
		const char            *bg_color         = nullptr; ///< 背景色（CSS 文字列）
		const char            *text_color       = nullptr; ///< 文字色（CSS 文字列）
	};

	obs_property_t *obs_properties_add_color_button_row(
		obs_properties_t         *props,
		const char               *prop_name,
		const char               *label,
		const ObsColorButtonSpec *buttons,
		size_t                    button_count); ///< OBS プロパティへ色付きボタン行を追加する

	void schedule_color_button_row_inject(obs_source_t *source); ///< 色付きボタン行の inject を UI スレッドへ予約する

} // namespace ods::widgets
