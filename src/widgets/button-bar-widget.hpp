#pragma once

#include <cstddef>
#include <obs-module.h>

namespace ods::widgets {

	/**
	 * ボタンバー行の 1 ボタン定義。
	 * 左寄せ・右寄せそれぞれ任意個数のボタンを配置できる。
	 */
	struct ObsButtonBarSpec {
		const char            *action_prop_name = nullptr; ///< クリック実体プロパティ名
		const char            *button_label     = nullptr; ///< ボタン表示ラベル
		obs_property_clicked_t clicked          = nullptr; ///< クリックコールバック
		void                  *clicked_priv     = nullptr; ///< コールバック引数
		bool                   enabled          = true;    ///< 初期有効状態
	};

	/// OBS プロパティへ左寄せ・右寄せのボタンバー行を追加する。
	/// 配列が nullptr の場合はそちら側にボタンを置かない。
	obs_property_t *obs_properties_add_button_bar(
		obs_properties_t       *props,
		const char             *prop_name,
		const char             *label,
		const ObsButtonBarSpec *left_buttons,
		size_t                  left_count,
		const ObsButtonBarSpec *right_buttons,
		size_t                  right_count);

	void schedule_button_bar_inject(obs_source_t *source); ///< ボタンバー行の inject を UI スレッドへ予約する

} // namespace ods::widgets
