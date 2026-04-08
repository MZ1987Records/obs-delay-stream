#pragma once

#include <QColor>
#include <QPalette>

namespace ods::ui {

	/**
	 * URL配布・遅延テーブルで共有するテーブル配色。
	 */
	struct TableThemeColors {
		QColor table_bg;    ///< テーブル全体背景
		QColor header_bg;   ///< ヘッダー背景
		QColor header_text; ///< ヘッダーテキスト
		QColor row_bg;      ///< 通常行背景
		QColor alt_row_bg;  ///< 交互行背景
		QColor text;        ///< 通常テキスト
		QColor border;      ///< 罫線
		QColor link;        ///< リンク
	};

	/// 交互行の視認差が不足する場合は代替色を使う。
	QColor pick_table_alt_row_color(const QColor &base, const QColor &alt_candidate);

	/// パレットからテーブル配色を組み立てる。
	TableThemeColors make_table_theme_colors(const QPalette &palette);

} // namespace ods::ui
