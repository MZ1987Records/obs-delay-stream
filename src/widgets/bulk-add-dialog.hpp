#pragma once

#include <obs-module.h>

#include <optional>
#include <string>
#include <vector>

class QWidget;

namespace ods::widgets {

	/**
	 * 出演者一括追加ダイアログの結果。
	 */
	struct BulkAddResult {
		std::vector<std::string> names;       ///< trim・空行除去・truncate 済みの出演者名リスト
		bool                     replace_all; ///< true: 既存全削除→新規追加、false: 空きスロットへ追加
	};

	/// 一括追加ダイアログをモーダル表示する。
	/// @param parent         親ウィジェット（nullptr 可）
	/// @param existing_count 現在のチャンネル数（カウンタ表示・上限判定用）
	/// @param max_count      上限チャンネル数（MAX_SUB_CH）
	/// @return ユーザーが OK を押した場合のみ結果を返す。Cancel/閉じた場合は std::nullopt
	std::optional<BulkAddResult> bulk_add_dialog_exec(
		QWidget *parent,
		int      existing_count,
		int      max_count);

} // namespace ods::widgets
