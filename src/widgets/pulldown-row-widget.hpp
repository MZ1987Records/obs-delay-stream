#pragma once

#include <cstddef>
#include <cstdint>
#include <obs-module.h>

namespace ods::widgets {

	/**
	 * プルダウン項目 1 件分の定義（整数値）。
	 */
	struct ObsPulldownOptionSpec {
		const char *item_label = nullptr; ///< 表示ラベル
		int64_t     item_value = 0;       ///< 設定へ保存する整数値
	};

	/**
	 * 横並びプルダウン 1 つ分の定義。
	 */
	struct ObsPulldownSpec {
		const char                  *list_prop_name = nullptr; ///< 隠し list プロパティ名（設定キー）
		const char                  *pulldown_label = nullptr; ///< 各プルダウンの見出し
		const ObsPulldownOptionSpec *options        = nullptr; ///< 選択肢配列
		size_t                       option_count   = 0;       ///< 選択肢数
		obs_property_modified2_t     modified       = nullptr; ///< 変更時コールバック
		void                        *modified_priv  = nullptr; ///< コールバック引数
		bool                         enabled        = true;    ///< 初期有効状態
		bool                         store_as_bool  = false;   ///< true の場合は bool 設定として保存する
	};

	obs_property_t *obs_properties_add_pulldown_row(
		obs_properties_t      *props,
		const char            *prop_name,
		const char            *label,
		const ObsPulldownSpec *pulldowns,
		size_t                 pulldown_count); ///< OBS プロパティへ横並びプルダウン行を追加する

	void schedule_pulldown_row_inject(obs_source_t *source); ///< 横並びプルダウン行の inject を UI スレッドへ予約する

} // namespace ods::widgets
