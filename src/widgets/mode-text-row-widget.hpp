#pragma once

#include <cstddef>
#include <cstdint>
#include <obs-module.h>

namespace ods::widgets {

	/**
	 * モード選択プルダウン項目 1 件分の定義（整数値）。
	 */
	struct ObsModeTextRowOptionSpec {
		const char *item_label = nullptr; ///< 表示ラベル
		int64_t     item_value = 0;       ///< 設定へ保存する整数値
	};

	/**
	 * プルダウン + 文字列入力を 1 行で扱う定義。
	 */
	struct ObsModeTextRowSpec {
		const char                     *mode_setting_key   = nullptr; ///< 隠し list プロパティ名（設定キー）
		const char                     *text_setting_key   = nullptr; ///< 隠し text プロパティ名（設定キー）
		const ObsModeTextRowOptionSpec *options            = nullptr; ///< 選択肢配列
		size_t                          option_count       = 0;       ///< 選択肢数
		int64_t                         manual_mode_value  = 0;       ///< 入力欄を有効化するモード値
		bool                            mode_store_as_bool = false;   ///< true の場合は bool 設定として保存する
		obs_property_modified2_t        mode_modified      = nullptr; ///< モード変更時コールバック
		void                           *mode_modified_priv = nullptr; ///< モード変更コールバック引数
		obs_property_modified2_t        text_modified      = nullptr; ///< 入力変更時コールバック
		void                           *text_modified_priv = nullptr; ///< 入力変更コールバック引数
		bool                            text_enabled       = true;    ///< 入力欄の初期有効状態
		int                             max_input_chars    = 0;       ///< 入力欄最大文字数（0 以下で無制限）
	};

	obs_property_t *obs_properties_add_mode_text_row(
		obs_properties_t         *props,
		const char               *prop_name,
		const char               *label,
		const ObsModeTextRowSpec &spec); ///< OBS プロパティへ横並びモード入力行を追加する

	void schedule_mode_text_row_inject(obs_source_t *source); ///< 横並びモード入力行の inject を UI スレッドへ予約する

} // namespace ods::widgets
